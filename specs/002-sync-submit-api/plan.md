# Implementation Plan: Synchronous Blocking Submit API

**Branch**: `002-sync-submit-api` | **Date**: 2026-03-15 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/002-sync-submit-api/spec.md`

## Summary

Add `Pipeline::submit_sync(Event, std::stop_token = {}) → std::expected<Decision, SubmitSyncError>` — a blocking in-process API that submits a single event, waits for the pipeline to produce a Decision (within the configured latency budget), and returns it directly to the caller. Emission targets still fire on success. On error (timeout, rate-limit, cancellation, etc.) a typed `SubmitSyncError` is returned and no emission occurs. The implementation reuses the existing `run_event()` coroutine with an optional `shared_ptr<SyncContext>` parameter; `std::condition_variable_any::wait_for` provides the cancellable blocking wait.

## Technical Context

**Language/Version**: C++23 (GCC 14+ / Clang 18+)
**Primary Dependencies**: Asio 1.30.2 (strand dispatch — unchanged), quill 4.5.0 (logging — unchanged), Catch2 3.7.1 (tests — unchanged). No new dependencies.
**Storage**: N/A
**Testing**: Catch2 3.7.1; `ctest --preset debug`
**Target Platform**: Linux / macOS (embedded library)
**Project Type**: C++ header+source library
**Performance Goals**: `submit_sync()` P99 latency ≤ pipeline `latency_budget` (constitution §VI; default 300 ms). Cancellation unblocks within one scheduling quantum (SC-006).
**Constraints**: No new external dependencies (constitution §III). Zero regression in existing 56 tests (SC-005).
**Scale/Scope**: Same concurrency scale as async submit — 1 000 concurrent callers per SC-003.

## Constitution Check

| Gate | Status | Notes |
|---|---|---|
| I. Spec-first | ✅ PASS | `spec.md` complete with acceptance scenarios |
| II. Test-first | ✅ REQUIRED | Tests in tasks before implementation |
| III. Minimal Dependencies | ✅ PASS | `std::stop_token`, `std::condition_variable_any` are stdlib; zero new external deps |
| IV. Backward Compatibility | ✅ PASS | New method with default arg; MINOR bump (1.0.0 → 1.1.0) |
| V. Simplicity | ✅ PASS | Single new method, private `SyncContext` struct, minimal run_event() diff |
| VI. Resiliency & Performance | ✅ PASS — see Failure Mode Analysis below | P99 bounded by latency budget; failure modes documented |

**Version bump**: 1.0.0 → 1.1.0 (MINOR — new backward-compatible public API)

## Failure Mode Analysis (Constitution §VI)

| What can fail | Blast radius | Graceful degradation |
|---|---|---|
| Pipeline destroyed while `submit_sync()` in-flight | Single caller | `SyncContext` is `shared_ptr`; coroutine holds a ref. `condition_variable_any` will be notified by coroutine or will timeout naturally. No dangling. |
| Evaluator or stage throws / hangs | Single event | Existing `FailureMode` (FailOpen/FailClosed/EmitDegraded) applies; coroutine always reaches `on_event_complete()` and notifies `SyncContext`. Worst case: timeout fires. |
| Cancellation after Decision produced | Single caller | `condition_variable_any::wait_for` returns immediately with the Decision; `Cancelled` is never returned once result is ready (FR-014). |
| Cancellation flag set before call | Single caller | Check stop_token immediately at call entry; return `Cancelled` without submitting (FR-013). |
| Rate limit exhausted | Single caller | Upfront `try_acquire()` check; immediate `RateLimited` return; does not block. |
| All N threads block simultaneously under low `max_concurrent` | N - max_concurrent callers | Excess callers return `RateLimited` immediately; accepted callers proceed. Blast radius bounded by existing concurrency cap. |
| `condition_variable_any` spurious wake | Single caller | Predicate (`done == true`) re-checked on wake; spurious wake re-enters wait. |

**Shuffle sharding / noisy-tenant isolation**: `submit_sync()` routes through the same `TenantRouter` as `submit()`. Per-tenant concurrency cap (`max_concurrent`) and token bucket are unchanged — each tenant's blocking calls are bounded independently. No cross-tenant blast radius.

## Project Structure

### Documentation (this feature)

```text
specs/002-sync-submit-api/
├── plan.md              ← this file
├── research.md          ← Phase 0 ✅
├── data-model.md        ← Phase 1 ✅
├── quickstart.md        ← Phase 1 ✅
├── contracts/
│   └── sync-submit-api.md  ← Phase 1 ✅
└── tasks.md             ← /speckit.tasks (not yet created)
```

### Source Code (repository root)

```text
include/fre/pipeline/
├── pipeline.hpp              # add submit_sync() declaration
├── pipeline_config.hpp       # unchanged
└── sync_submit.hpp           # NEW: SubmitSyncError enum

src/pipeline/
└── pipeline.cpp              # implement submit_sync(); extend run_event()

tests/unit/
├── test_sync_submit.cpp      # NEW: unit tests (timeout, cancellation, not-started, etc.)
└── CMakeLists.txt            # add fre_unit_sync_submit target

tests/integration/
├── test_sync_submit_integration.cpp  # NEW: coexistence, concurrent mixed load
└── CMakeLists.txt            # add fre_integration_sync_submit target
```

**Structure Decision**: Single-project layout. New files are drop-in additions to the existing `include/fre/pipeline/` and `src/pipeline/` trees. No new directories except the test files.

## Implementation Phases

### Phase 1 — New header and error type
- Create `include/fre/pipeline/sync_submit.hpp` with `SubmitSyncError` enum

### Phase 2 — Internal SyncContext + run_event() extension
- Add private `SyncContext` struct in `src/pipeline/pipeline.cpp`
- Add optional `shared_ptr<SyncContext>` param to `run_event()` (default nullptr)
- After emit stage in coroutine: if ctx non-null, store Decision and notify cv

### Phase 3 — submit_sync() implementation
- Declare in `pipeline.hpp`
- Implement in `pipeline.cpp`:
  1. Check stop_token (pre-call cancellation → `Cancelled`)
  2. Check pipeline state (not running → `PipelineUnavailable` / `NotStarted`)
  3. `try_acquire()` (rate-limit → `RateLimited`)
  4. Allocate `SyncContext`, spawn `run_event()` with it
  5. `cv.wait_for(lk, cancel, latency_budget, [&]{ return ctx->done; })`
  6. If stop_token requested → `Cancelled`
  7. If timeout → `Timeout`
  8. Else → return `ctx->decision.value()`
- Log entry/exit structured log lines

### Phase 4 — Tests (written before Phase 1-3 code per constitution §II)

**Unit tests** (`tests/unit/test_sync_submit.cpp`):
- Returns Decision on success
- Returns Timeout when stages exceed budget
- Returns RateLimited when concurrency cap exhausted
- Returns PipelineUnavailable when pipeline is draining
- Returns NotStarted before pipeline.start()
- Returns ValidationFailed for invalid event
- Returns Cancelled when stop_token set before call
- Returns Cancelled when stop_token set during wait
- Returns Decision (not Cancelled) when stop_token set after decision produced
- Emission targets fire on success
- Emission targets do NOT fire on error

**Integration tests** (`tests/integration/test_sync_submit_integration.cpp`):
- Mixed async + blocking concurrent load (1 000 events, no cross-contamination)
- N concurrent submit_sync() calls under low max_concurrent (excess → RateLimited)
- Existing fre_integration_* tests all pass unchanged

## Complexity Tracking

> No constitution violations. No complexity justified here — the implementation is a straightforward extension of the existing coroutine pattern.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| — | — | — |
