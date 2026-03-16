# Research: Synchronous Blocking Submit API

**Feature**: 002-sync-submit-api
**Date**: 2026-03-15

---

## Decision 1: Blocking wait mechanism

**Decision**: Use `std::condition_variable_any` + `std::mutex` inside a heap-allocated `SyncContext` shared between the calling thread and the pipeline's coroutine via `std::shared_ptr`.

**Rationale**: `std::future::wait_for()` cannot be interrupted by a `std::stop_token`. `std::condition_variable_any::wait_for(lock, stop_token, timeout, pred)` is the C++20 overload that wakes on *any* of: predicate true, timeout elapsed, or stop requested — exactly the three exit conditions required (decision ready, latency budget expired, cancellation flag set). `PipelineTestHarness` already uses this pattern (`std::mutex` + `std::condition_variable`) confirming it is idiomatic in this codebase.

**Alternatives considered**:
- `std::promise<Decision>` + `std::future<Decision>`: rejected — `std::future::wait_for` has no stop_token overload; adding cancellation requires a second synchronisation object, negating the simplicity benefit.
- `asio::use_future` completion token: rejected — requires changing the executor model and introduces asio into the public call path; inconsistent with Principle III (Minimal Dependencies).
- Polling loop with short `wait_for` intervals: rejected — busy-wait-adjacent, wastes CPU, and violates the SC-006 "one scheduling quantum" unblock requirement.

---

## Decision 2: How to deliver the Decision to the blocking caller

**Decision**: Add an optional `std::shared_ptr<SyncContext>` parameter to the private `run_event()` coroutine. After the emit stage completes (and registered emission targets have already fired, satisfying FR-007), `run_event()` checks if the context is non-null and, if so, stores the `Decision` and notifies the `condition_variable_any`.

**Rationale**: This is the minimal-diff approach. The existing `run_event()` already has the `Decision` in scope after the emit stage — no restructuring needed. Emission targets still fire first (FR-007 compliance). The `SyncContext` is allocated on the heap and owned by both the calling thread and the coroutine lambda via `shared_ptr`, so pipeline destruction while in-flight is safe: the coroutine holds a reference, preventing dangling.

**Alternatives considered**:
- Inject a one-shot `EmissionFn` per-event by modifying `EmitStageConfig` at runtime: rejected — the emit stage's target list is shared across all concurrent events; concurrent modification is a data race without additional locking, and locking would serialise all emissions.
- Add a callback field to `Event`: rejected — pollutes the public `Event` struct with an internal implementation detail; violates Principle V (Simplicity) and Principle IV (API cleanliness).
- Add a parallel `run_event_sync()` coroutine: rejected — code duplication of all stage processing logic; maintenance burden exceeds benefit.

---

## Decision 3: Cancellation API surface

**Decision**: Accept `std::stop_token` as the cancellation parameter (defaulting to `{}` — an empty, never-requested token). This is the C++20-standard cancellation primitive, requiring no new dependencies.

**Rationale**: `std::stop_token` integrates directly with `std::condition_variable_any::wait_for`, producing exactly the desired semantics: when the caller calls `source.request_stop()`, the blocking wait exits and returns `SubmitSyncError::Cancelled`. The default `{}` makes the parameter completely optional for callers who don't need cancellation.

**Alternatives considered**:
- Custom `CancellationFlag` wrapper around `std::atomic<bool>`: rejected — requires a polling or notification mechanism to be built from scratch; `std::stop_token` already provides this via `std::stop_callback`.
- `std::future<void>` cancellation signal: rejected — no standard way to interrupt `std::condition_variable_any::wait_for` with a future.

---

## Decision 4: Error type representation

**Decision**: Create a new `SubmitSyncError` enum in `include/fre/pipeline/sync_submit.hpp`. The `submit_sync()` method returns `std::expected<Decision, SubmitSyncError>`. This is separate from the existing `Error` variant to avoid widening the general error type with sync-specific codes.

**Rationale**: `SubmitSyncError` codes (Timeout, RateLimited, PipelineUnavailable, NotStarted, ValidationFailed, Cancelled) are meaningful only to blocking callers. Folding them into the existing `std::variant<ConfigError, EvaluatorError, ...>` would make the general error type carry sync-specific baggage, violating Principle V. The existing `PipelineError` codes map to `PipelineUnavailable` and `NotStarted` internally but are translated at the `submit_sync()` boundary.

**Alternatives considered**:
- Extend `PipelineErrorCode` with `SyncTimeout`, `SyncCancelled`: rejected — mixes general pipeline errors with synchronous-call-specific outcomes; confuses callers of the async path.
- Return the existing `Error` variant: rejected — callers would need to `std::visit` a complex variant to handle simple "timed out" vs "rate limited" outcomes.

---

## Decision 5: Thread safety strategy

**Decision**: `submit_sync()` performs upfront state and rate-limit checks using the same atomic + rate-limit path as `submit()`. The `SyncContext` uses its own `std::mutex` + `std::condition_variable_any`; no locking of pipeline-global state is added. Each in-flight `submit_sync()` call is completely independent.

**Rationale**: The existing `submit()` is already thread-safe via atomics and `TenantRouter`. Reusing the same upfront checks ensures consistent rate-limiting behaviour (FR-003 compliance). Per-`SyncContext` locking means N concurrent `submit_sync()` callers never contend on a shared mutex — scalability is preserved.

---

## Decision 6: Version bump

**Decision**: MINOR version bump (e.g., 1.0.0 → 1.1.0). `submit_sync()` is a new public API method with a default-argument cancellation parameter — purely additive, no existing API changed.

**Rationale**: Semantic Versioning Principle IV requires MINOR for new backward-compatible functionality. The default `std::stop_token{}` parameter means existing call sites compile without modification.

---

## Decision 7: Observability

**Decision**: The same quill structured log calls made inside `run_event()` apply to sync submissions without modification. Add one additional log entry at the `submit_sync()` call site: a `LOG_INFO` on entry (event received via sync path) and a `LOG_INFO` on exit (decision returned or error code). This distinguishes sync from async in logs without duplicating stage-level instrumentation.

**Rationale**: `run_event()` already emits stage-transition and decision audit log lines. Duplicating these in `submit_sync()` would produce double entries. A single boundary-level log at the sync call site is sufficient to make the sync path visible to operators.
