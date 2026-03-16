# Data Model: Synchronous Blocking Submit API

**Feature**: 002-sync-submit-api
**Date**: 2026-03-15

---

## Entities

### SubmitSyncError (new)

An enumeration of all typed failure reasons a blocking submit can return. Each variant is mutually exclusive and unambiguous to callers.

| Variant | Condition |
|---|---|
| `Timeout` | Decision not produced within the pipeline's latency budget |
| `RateLimited` | Rate-limit or concurrency cap exhausted at submission time |
| `PipelineUnavailable` | Pipeline is in `Draining` or `Stopped` state at submission time |
| `NotStarted` | Pipeline has never been started (`Starting` or `Stopped` before first `start()`) |
| `ValidationFailed` | Event failed ingest-stage validation (missing required fields, clock skew) |
| `Cancelled` | Caller set the `std::stop_token` before or during the blocking wait |

**Location**: `include/fre/pipeline/sync_submit.hpp`

---

### SyncContext (internal, not public)

A heap-allocated coordination object shared between the calling thread and the `run_event()` coroutine. Lifetime is managed by `std::shared_ptr`; safe across pipeline destruction.

| Field | Type | Purpose |
|---|---|---|
| `mtx` | `std::mutex` | Guards `decision` and `done` |
| `cv` | `std::condition_variable_any` | Wakes the blocking caller when done or cancelled |
| `decision` | `std::optional<Decision>` | Filled by `run_event()` after emit stage |
| `done` | `bool` | Predicate for the `wait_for` loop |

**Location**: `src/pipeline/pipeline.cpp` (private, translation-unit only)

---

### Decision (existing, unchanged)

The existing output of the pipeline. A successful `submit_sync()` returns this by value. Content is identical to what emission targets receive (FR-011).

See `include/fre/core/decision.hpp` for the full definition.

---

### Event (existing, unchanged)

Input to both `submit()` and `submit_sync()`. No new fields added.

See `include/fre/core/event.hpp`.

---

## State Transitions

`submit_sync()` accepts the event only when the pipeline is in the `Running` state. All other states map to an immediate error:

```
PipelineState::Stopped   → SubmitSyncError::PipelineUnavailable
PipelineState::Starting  → SubmitSyncError::PipelineUnavailable
PipelineState::Running   → proceed to rate-limit check
PipelineState::Draining  → SubmitSyncError::PipelineUnavailable
```

The `NotStarted` error is returned when `start()` has never been called (i.e., `state_ == Stopped` and the pipeline has never transitioned to `Running`). In the current implementation this is indistinguishable from post-drain Stopped at the state machine level; the `NotStarted` code is returned for both to satisfy SC-004.

> **Implementation note**: The spec distinguishes `NotStarted` from `PipelineUnavailable` at the API level. Internally, both map to `PipelineState::Stopped`/`Starting` checks. Both are returned as `SubmitSyncError::PipelineUnavailable` unless the pipeline has never been started, in which case `NotStarted` is used. The implementation may track a `bool ever_started_` flag to distinguish the two.

---

## Relationships

```
Pipeline
  │
  ├── submit(Event) → std::expected<void, Error>            [existing async]
  │
  └── submit_sync(Event, std::stop_token = {})
            → std::expected<Decision, SubmitSyncError>       [new blocking]
                    │
                    ├── on success  → Decision (identical to emission target delivery)
                    └── on failure  → SubmitSyncError
```

`submit_sync()` internally re-uses `run_event()` (the same coroutine as `submit()`), passing a `shared_ptr<SyncContext>`. The coroutine notifies the context after the emit stage — ensuring emission targets have already fired before the caller unblocks.
