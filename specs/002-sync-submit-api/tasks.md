# Tasks: Synchronous Blocking Submit API

**Input**: Design documents from `/specs/002-sync-submit-api/`
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, contracts/ ✅, quickstart.md ✅

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.
**TDD**: Per Constitution §II (Non-Negotiable), test tasks MUST be written and confirmed failing before the corresponding implementation tasks.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no shared dependencies)
- **[Story]**: Which user story this task belongs to
- All paths are relative to repo root

---

## Phase 1: Setup

**Purpose**: Create the new header, stub test files, and register CMake test targets so the TDD cycle can begin.

- [X] T001 Create `include/fre/pipeline/sync_submit.hpp` with the `SubmitSyncError` enum (6 variants: Timeout, RateLimited, PipelineUnavailable, NotStarted, ValidationFailed, Cancelled) and `namespace fre` as specified in `specs/002-sync-submit-api/contracts/sync-submit-api.md`
- [X] T002 [P] Create stub `tests/unit/test_sync_submit.cpp` (empty Catch2 file with includes) and add `fre_unit_sync_submit` test target to `tests/unit/CMakeLists.txt` using the `fre_add_test()` helper with `TIMEOUT 60`
- [X] T003 [P] Create stub `tests/integration/test_sync_submit_integration.cpp` (empty Catch2 file with includes) and add `fre_integration_sync_submit` test target to `tests/integration/CMakeLists.txt` using the `fre_add_test()` helper with `TIMEOUT 120`

**Checkpoint**: `cmake --preset debug && cmake --build --preset debug` succeeds with new (empty) test binaries present.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Internal coordination infrastructure that all user story implementations depend on. MUST be complete before any `submit_sync()` implementation begins.

**⚠️ CRITICAL**: US1–US3 implementation cannot begin until this phase is complete.

- [X] T004 Add a private `SyncContext` struct inside `src/pipeline/pipeline.cpp` (translation-unit scope, not public) with fields: `std::mutex mtx`, `std::condition_variable_any cv`, `std::optional<Decision> decision`, `std::optional<SubmitSyncError> error`, `std::atomic<bool> cancelled{false}`, `bool done{false}` — the `error` field is populated by `run_event()` when ingest validation fails (FR-006, FR-013); see `specs/002-sync-submit-api/data-model.md` for field descriptions
- [X] T005 Extend the private `run_event()` coroutine in `src/pipeline/pipeline.cpp` to accept an optional `std::shared_ptr<SyncContext> sync_ctx = nullptr` parameter; after the emit stage completes and before `on_event_complete()`, if `sync_ctx` is non-null and `sync_ctx->cancelled` is false, store the Decision in `sync_ctx->decision`, set `sync_ctx->done = true`, and call `sync_ctx->cv.notify_one()`; if `sync_ctx->cancelled` is true, skip the emit stage entirely (no emission on cancelled/timed-out paths) and just call `on_event_complete()`

**Checkpoint**: Codebase compiles cleanly. Existing 56 tests still pass (`ctest --preset debug`).

---

## Phase 3: User Story 1 — Blocking Submit Returns Decision (Priority: P1) 🎯 MVP

**Goal**: A caller can invoke `pipeline.submit_sync(event)` and receive a `Decision` directly, with no emission-target wiring required. Emission targets also fire on success.

**Independent Test**: `fre_unit_sync_submit` — submit one event to a running pipeline via `submit_sync()`, assert Decision is returned with correct verdict, assert emission target received identical Decision.

### Tests for User Story 1

> **Write these tests FIRST. Confirm they FAIL (compile but assert-fail) before T007.**

- [X] T006 [US1] Write failing unit tests for US1 happy path in `tests/unit/test_sync_submit.cpp`: (a) `submit_sync` returns `Decision` with correct verdict, (b) returned `Decision` content is identical to what a registered counting emission target receives, (c) elapsed wall-clock time is within pipeline latency budget — use a pipeline with `RateLimitConfig{100'000, 200'000, 10'000}` and `latency_budget(300ms)`

### Implementation for User Story 1

- [X] T007 [US1] Declare `submit_sync(Event, std::stop_token = {}) -> std::expected<Decision, SubmitSyncError>` in `include/fre/pipeline/pipeline.hpp`; implement in `src/pipeline/pipeline.cpp`: (1) check `cancel.stop_requested()` → return `Cancelled`; (2) check `state_ != Running` → return appropriate error; (3) `try_acquire(event.tenant_id)` → return `RateLimited` on failure; (4) allocate `shared_ptr<SyncContext>`; (5) `asio::co_spawn(strand, run_event(event, ctx), asio::detached)`; (6) `ctx->cv.wait_for(lk, cancel, latency_budget_, [&]{ return ctx->done; })`; (7) return `ctx->decision.value()` on success
- [X] T008 [US1] Add structured `LOG_INFO` entry line (event received via sync path, tenant_id, entity_id) and exit line (decision returned or error code, elapsed_us) in `submit_sync()` in `src/pipeline/pipeline.cpp` using the existing quill logger pattern

**Checkpoint**: `fre_unit_sync_submit` happy-path tests pass. US1 acceptance scenarios verified.

---

## Phase 4: User Story 2 — Timeout and Error Propagation (Priority: P2)

**Goal**: Every error condition returns a distinct, typed `SubmitSyncError` variant promptly. No error path emits a decision to emission targets.

**Independent Test**: `fre_unit_sync_submit` error-variant tests — induce each of the 6 error conditions and assert the correct variant is returned within the expected time bound; assert zero emission target calls on each error path.

### Tests for User Story 2

> **Write these tests FIRST. Confirm they FAIL before T010.**

- [X] T009 [US2] Write failing unit tests for all 6 error variants in `tests/unit/test_sync_submit.cpp`: (a) `Timeout` — pipeline with a 10ms latency budget and a slow evaluator that sleeps 50ms; (b) `RateLimited` — pipeline with `max_concurrent=1`, hold one slot, assert second call returns `RateLimited` immediately; (c) `PipelineUnavailable` — call on a draining pipeline; (d) `NotStarted` — call before `pipeline.start()`; (e) `ValidationFailed` — submit event with empty `tenant_id`; (f) `Cancelled` pre-call — set stop_token before calling, assert `Cancelled` returned without submitting; (g) `Cancelled` in-flight — set stop_token from separate thread during wait; (h) no-emission on each error path — use a counting emission target and assert count stays 0

### Implementation for User Story 2

- [X] T010 [US2] Implement `NotStarted` and `PipelineUnavailable` error paths in `submit_sync()` in `src/pipeline/pipeline.cpp`: when `state_ == Stopped` and pipeline has never been started (track with `bool ever_started_` atomic or flag set in `start()`) return `NotStarted`; when `state_ == Draining` or `Stopped` post-drain return `PipelineUnavailable`
- [X] T011 [US2] Implement `RateLimited` error path in `submit_sync()` in `src/pipeline/pipeline.cpp`: translate `RateLimitError` from `try_acquire()` to `SubmitSyncError::RateLimited`
- [X] T012 [US2] Implement `Timeout` error path in `submit_sync()` in `src/pipeline/pipeline.cpp`: when `wait_for` returns `false` (timeout without `done`), set `ctx->cancelled = true` to suppress emission in the still-running coroutine, return `SubmitSyncError::Timeout`
- [X] T013 [US2] Implement `ValidationFailed` error mapping in `submit_sync()` in `src/pipeline/pipeline.cpp`: `run_event()` already calls the ingest stage which returns a stage error on invalid events; pass this back through `SyncContext` by storing an `std::optional<SubmitSyncError>` error field in `SyncContext` and having `run_event()` populate it before notifying the cv when ingest fails (and skip remaining stages + emission)
- [X] T014 [US2] Implement `Cancelled` paths in `submit_sync()` in `src/pipeline/pipeline.cpp`: (a) pre-call: `if (cancel.stop_requested()) return SubmitSyncError::Cancelled;` as the first statement; (b) in-flight: after `cv.wait_for`, check `cancel.stop_requested()` — if true, set `ctx->cancelled = true` and return `SubmitSyncError::Cancelled`
- [X] T014b [US2] Write and pass a unit test for the "pipeline destroyed while `submit_sync()` in-flight" safety property in `tests/unit/test_sync_submit.cpp`: submit an event with a slow evaluator (50ms sleep), destroy the `Pipeline` object from another thread after 10ms (while the blocking call is waiting), assert the blocking call returns an error (Timeout or PipelineUnavailable) with no crash, ASAN/LSAN report no leak or use-after-free — this verifies the `shared_ptr<SyncContext>` lifetime claim in the failure mode analysis

**Checkpoint**: All `fre_unit_sync_submit` tests pass including all error-variant, no-emission, and in-flight-destruction assertions. US2 acceptance scenarios verified.

---

## Phase 5: User Story 3 — Coexistence with Async Submit (Priority: P3)

**Goal**: `submit()` and `submit_sync()` on the same pipeline instance operate concurrently without interference. Rate limiting applies uniformly to both.

**Independent Test**: `fre_integration_sync_submit` — submit 500 async and 500 blocking events concurrently, assert all 1000 decisions are produced with correct tenant/entity correlation (no cross-contamination), assert rate-limit counters are consistent.

### Tests for User Story 3

> **Write these tests FIRST. Confirm they FAIL before T016.**

- [X] T015 [US3] Write failing integration tests in `tests/integration/test_sync_submit_integration.cpp`: (a) mixed concurrent load — 10 threads each submitting 50 async + 50 blocking events to the same pipeline; assert all 1000 decisions have correct `tenant_id`/`entity_id` (no cross-contamination); use `RateLimitConfig{100'000, 200'000, 10'000}` and `latency_budget(300ms)`; (b) shared rate-limit pool — pipeline with `max_concurrent=5`, submit 10 concurrent `submit_sync()` calls simultaneously, assert at most 5 succeed and remainder return `RateLimited`

### Implementation for User Story 3

- [X] T016 [US3] Verify `TenantRouter::try_acquire()` is called identically in both `submit()` and `submit_sync()` paths in `src/pipeline/pipeline.cpp` — no code changes expected; if any discrepancy is found, fix the `submit_sync()` path to mirror `submit()` rate-limit acquisition and release exactly

**Checkpoint**: `fre_integration_sync_submit` passes. US3 acceptance scenarios verified.

---

## Phase 6: Polish & Cross-Cutting Concerns

- [X] T017 [P] Update `README.md`: add `submit_sync()` to the "Using as a Library" section with a minimal example (adapted from `specs/002-sync-submit-api/quickstart.md` Scenario 1) and an error-handling reference table
- [X] T018 [P] Bump version to `1.1.0` in `CMakeLists.txt` (the `project()` call `VERSION` field); update `CHANGELOG.md` (create if absent) with a `[1.1.0]` section noting the new `submit_sync()` API
- [X] T019 Run `ctest --preset debug` for the full test suite and confirm all tests pass (existing 56 + new sync-submit tests) to satisfy SC-005 (no regression)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately; T002 and T003 can run in parallel
- **Foundational (Phase 2)**: Depends on T001 (sync_submit.hpp must exist for pipeline.cpp includes); T004 and T005 must run sequentially (same file)
- **US1 (Phase 3)**: Depends on Phase 2 complete; T006 (test) before T007 (impl) before T008 (logging)
- **US2 (Phase 4)**: Depends on Phase 3 complete (submit_sync() must exist to add error paths); T009 (tests) before T010–T014 (impl, sequential — same file); T014b (safety test) after T014
- **US3 (Phase 5)**: Depends on Phase 4 complete; T015 (tests) before T016 (verify)
- **Polish (Phase 6)**: Depends on Phase 5 complete; T017 and T018 can run in parallel; T019 last

### User Story Dependencies

- **US1 (P1)**: Requires Phase 2 complete — no other story dependency
- **US2 (P2)**: Requires US1 complete (error paths extend the same `submit_sync()` function)
- **US3 (P3)**: Requires US2 complete (tests validate correctness of the full implementation)

### Parallel Opportunities

- T002 + T003 (Phase 1): different files, run together
- T017 + T018 (Phase 6): different files, run together
- T010, T011, T012, T013, T014, T014b (Phase 4 impl): all modify `pipeline.cpp` or its test file — must be sequential

---

## Parallel Example: Phase 1

```
Simultaneously:
  T002 — tests/unit/test_sync_submit.cpp + tests/unit/CMakeLists.txt
  T003 — tests/integration/test_sync_submit_integration.cpp + tests/integration/CMakeLists.txt
```

## Parallel Example: Phase 6

```
Simultaneously:
  T017 — README.md
  T018 — CMakeLists.txt version bump + CHANGELOG.md
Then:
  T019 — ctest --preset debug
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 (Setup) — 3 tasks
2. Complete Phase 2 (Foundational) — 2 tasks
3. Complete Phase 3 (US1) — 3 tasks
4. **STOP and VALIDATE**: `./build/debug/tests/unit/fre_unit_sync_submit --reporter console -v high`
5. US1 alone is a fully shippable, independently useful increment

### Incremental Delivery

1. Phase 1 + 2 → Foundation ready
2. Phase 3 (US1) → Basic blocking submit works → MINOR release 1.1.0-rc1
3. Phase 4 (US2) → All error paths safe → MINOR release 1.1.0-rc2
4. Phase 5 (US3) → Concurrent coexistence verified → MINOR release 1.1.0
5. Phase 6 → Polish → Merge

---

## Notes

- Constitution §II (Test-First) is non-negotiable: each test task MUST produce failing tests before the corresponding implementation task runs
- `SyncContext` is a translation-unit-private struct — do not expose it in any header
- The `cancelled` flag in `SyncContext` is the key mechanism for FR-006 compliance on Timeout and Cancelled paths — `run_event()` checks it before the emit stage
- Use `std::condition_variable_any::wait_for(unique_lock, stop_token, duration, predicate)` — the C++20 overload that handles all three exit conditions in one call
- Rate-limit config for all new tests: `RateLimitConfig{100'000, 200'000, 10'000}` to avoid submit failures unrelated to the test's intent
