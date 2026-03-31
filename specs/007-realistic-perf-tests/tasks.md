# Tasks: Realistic Performance Tests with DuckDB

**Input**: Design documents from `/specs/007-realistic-perf-tests/`
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, quickstart.md ✓

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to ([US1], [US2])

---

## Phase 1: Setup (CMake Registration)

**Purpose**: Register both new test targets in CMake before writing any test code. Both targets are gated behind `if(FRE_ENABLE_DUCKDB)` blocks, mirroring the pattern for `fre_unit_write_back_window_store` and `fre_integration_write_back_pipeline`.

- [X] T001 [P] Add `fre_unit_realistic_latency_benchmark` target in `tests/unit/CMakeLists.txt` inside the existing `if(FRE_ENABLE_DUCKDB)` block: `add_executable`, link `fre::pipeline Catch2::Catch2WithMain`, `target_compile_options(-Wall -Wextra -Wpedantic)`, `catch_discover_tests` with `EXTRA_ARGS --benchmark-samples 3` and `PROPERTIES TIMEOUT 120`
- [X] T002 [P] Add `fre_integration_realistic_load_p99` target in `tests/integration/CMakeLists.txt` inside the existing `if(FRE_ENABLE_DUCKDB)` block: `add_executable`, link `fre::pipeline Catch2::Catch2WithMain`, `target_compile_options(-Wall -Wextra -Wpedantic)`, `catch_discover_tests` with `PROPERTIES TIMEOUT 180`

**Checkpoint**: `cmake --preset duckdb` configures without error (test binaries not yet buildable until source files exist).

---

## Phase 2: User Story 1 — Latency Benchmark with Real Evaluators (Priority: P1) 🎯 MVP

**Goal**: `tests/unit/test_realistic_latency_benchmark.cpp` — Catch2 `BENCHMARK` test that measures p50/p99 latency through `AllowDenyEvaluator` + `ThresholdEvaluator<WriteBackWindowStore>` with on-disk DuckDB. Post-benchmark REQUIRE assertions confirm at least one `Flag` and one `Block` verdict. `TempFileGuard` deletes the DuckDB file and deny-list file on scope exit.

**Independent Test**: `ctest --preset duckdb -R "fre_unit_realistic_latency_benchmark" --output-on-failure`

### Implementation for User Story 1

- [X] T003 [US1] Create `tests/unit/test_realistic_latency_benchmark.cpp` with file-level `#ifdef FRE_ENABLE_DUCKDB` guard, required includes (`<fre/evaluator/allow_deny_evaluator.hpp>`, `<fre/evaluator/threshold_evaluator.hpp>`, `<fre/state/duckdb_window_store.hpp>`, `<fre/state/write_back_window_store.hpp>`, `<fre/testing/pipeline_harness.hpp>`, `<catch2/benchmark/catch_benchmark.hpp>`, `<catch2/catch_test_macros.hpp>`, `<filesystem>`, `<fstream>`, `<unistd.h>`), and a `TempFileGuard` RAII struct that holds a `std::vector<std::filesystem::path>` and calls `std::filesystem::remove` for each path in its destructor; also add `write_file(path, content)` helper matching the `write_list_file` pattern in `tests/unit/test_allow_deny_evaluator.cpp`

- [X] T004 [US1] Add `NoOpInferenceEval` stub (returns `Verdict::Pass`, `evaluator_id = "noop_inf"`) and `make_bench_config(store, deny_list_path)` factory in `tests/unit/test_realistic_latency_benchmark.cpp`: build `EvalStageConfig` with `CompositionRule::AnyBlock`, `add_evaluator(AllowDenyEvaluator{AllowDenyEvaluatorConfig{.deny_list_path=deny_list_path, .match_field=EntityId, .default_verdict=Pass}})` then `add_evaluator(ThresholdEvaluator<WriteBackWindowStore>{ThresholdEvaluatorConfig{.window_duration=60s, .aggregation=Count, .group_by=EntityId, .threshold=200.0, .window_name="bench_threshold"}, store})`; add `InferenceStageConfig` with `NoOpInferenceEval` and `timeout=50ms`; set `RateLimitConfig{bucket_capacity=100'000, tokens_per_second=500'000, max_concurrent=20'000}`; call `PipelineConfig::Builder{}.pipeline_id("bench-realistic").rate_limit(...).eval_config(...).inference_config(...).emit_config(EmitStageConfig{}.add_noop_target()).build()`

- [X] T005 [US1] Implement `TEST_CASE("P99 latency benchmark — real evaluators + DuckDB", "[benchmark][realistic]")` in `tests/unit/test_realistic_latency_benchmark.cpp`: (1) construct `TempFileGuard guard` holding `std::filesystem::temp_directory_path() / ("fre_bench_" + std::to_string(getpid()) + ".duckdb")` and `... / ("fre_bench_deny_" + std::to_string(getpid()) + ".txt")`; (2) call `write_file(guard.paths[1], "blocked-entity\n")`; (3) create `DuckDbWindowStore warm{DuckDbConfig{.db_path=guard.paths[0], .flush_interval_ms=0, .window_ms=60000, .warm_epoch_retention=3}}`; (4) create `InProcessWindowStore primary`; (5) create `WriteBackWindowStore store{primary_ptr, warm_ptr, WriteBackConfig{.flush_interval_ms=200}}`; (6) call `make_bench_config(store_ptr, guard.paths[1])`, `REQUIRE(cfg.has_value())`; (7) construct `PipelineTestHarness harness{std::move(*cfg)}` and `REQUIRE(harness.start().has_value())`

- [X] T006 [US1] Add warm-up phase in the TEST_CASE body in `tests/unit/test_realistic_latency_benchmark.cpp`: submit 100 events with `tenant_id="bench-tenant"`, `entity_id="warmup-entity"`, `event_type="warmup"`; call `harness.wait_for_decisions(100, 5000ms)`; call `harness.clear_decisions()`

- [X] T007 [US1] Add `BENCHMARK("realistic pipeline: AllowDeny + Threshold + DuckDB")` body in `tests/unit/test_realistic_latency_benchmark.cpp`: `harness.clear_decisions()`; build 1000 events — 500 with `entity_id="high-volume-entity"` (crosses threshold=200 at event 201), 100 with `entity_id="blocked-entity"` (on deny list), 400 with `entity_id="normal-entity"` — all with `tenant_id="bench-tenant"`, `event_type="api_call"`, `timestamp=std::chrono::system_clock::now()`; `harness.submit_events(events)`; `auto span = harness.wait_for_decisions(1000, 30'000ms)`; collect `elapsed_us` into a sorted vector; compute `p99_idx = size * 99 / 100`; `return latencies[p99_idx]`

- [X] T008 [US1] Add post-`BENCHMARK` assertions and teardown in `tests/unit/test_realistic_latency_benchmark.cpp`: after the `BENCHMARK` macro, call `harness.drain(5000ms)`; access `harness.decisions()`; assert `std::any_of(..., [](auto& d){ return d.final_verdict == Verdict::Flag; })` is true; assert `std::any_of(..., [](auto& d){ return d.final_verdict == Verdict::Block; })` is true; add `#endif // FRE_ENABLE_DUCKDB` at end of file

**Checkpoint**: `ctest --preset duckdb -R "fre_unit_realistic_latency_benchmark" --output-on-failure` passes; Catch2 benchmark output shows non-zero p99; no DuckDB or deny-list temp files remain under `$TMPDIR` after completion.

---

## Phase 3: User Story 2 — Sustained Load Test with Per-Tenant DuckDB (Priority: P1)

**Goal**: `tests/integration/test_realistic_load_p99.cpp` — 10-tenant concurrent load test with `AllowDenyEvaluator` + `ThresholdEvaluator<WriteBackWindowStore>` backed by on-disk DuckDB. Asserts overall and per-tenant P99 ≤ 500ms, ≥1 `Flag` verdict per tenant, ≥1 `Block` verdict per tenant, and total decisions == 30 000. `TempFileGuard` cleans up all temp files unconditionally.

**Independent Test**: `ctest --preset duckdb -R "fre_integration_realistic_load_p99" --output-on-failure`

### Implementation for User Story 2

- [X] T009 [US2] Create `tests/integration/test_realistic_load_p99.cpp` with `#ifdef FRE_ENABLE_DUCKDB` guard, all required includes (same evaluator/state headers as US1, plus `<atomic>`, `<thread>`, `<mutex>`), and the same `TempFileGuard` RAII struct and `write_file(path, content)` helper as US1 (copy verbatim — no shared header needed)

- [X] T010 [US2] Add `NoOpInferenceEval` stub (same as T004 — returns `Verdict::Pass`, `evaluator_id = "noop_inf"`) and `make_load_config(store, deny_list_path)` factory in `tests/integration/test_realistic_load_p99.cpp`: `EvalStageConfig` with `CompositionRule::AnyBlock` containing `AllowDenyEvaluator` (deny_list_path, EntityId match) then `ThresholdEvaluator<WriteBackWindowStore>` (threshold=200, window=60s, window_name="load_threshold"); `InferenceStageConfig` with `NoOpInferenceEval` and `timeout=50ms` (satisfies FR-005); `RateLimitConfig{bucket_capacity=100'000, tokens_per_second=200'000, max_concurrent=10'000}`; `EmitStageConfig{}.add_noop_target()`; `PipelineConfig::Builder{}.pipeline_id("load-realistic").rate_limit(...).eval_config(...).inference_config(...).emit_config(...).build()`

- [X] T011 [US2] Implement `TEST_CASE("Realistic load: 10 tenants x 3000 events, P99 ≤ 500ms", "[integration][load][realistic]")` setup in `tests/integration/test_realistic_load_p99.cpp`: construct `TempFileGuard guard` with unique DuckDB path (`fre_load_<pid>.duckdb`) and deny-list path (`fre_load_deny_<pid>.txt`); build deny-list content with one entry per tenant: `"blocked-0\nblocked-1\n...\nblocked-9\n"`; `write_file(guard.paths[1], deny_content)`; create `DuckDbWindowStore`, `InProcessWindowStore`, `WriteBackWindowStore{primary, warm, {.flush_interval_ms=500}}`; call `make_load_config(store_ptr, guard.paths[1])`; `REQUIRE(cfg.has_value())`; `PipelineTestHarness harness{std::move(*cfg)}`; `REQUIRE(harness.start().has_value())`

- [X] T012 [US2] Add warm-up phase (100 events for `entity_id="warmup"`, `tenant_id="warmup-tenant"`, wait, clear) then concurrent submission loop in `tests/integration/test_realistic_load_p99.cpp`: launch 10 `std::thread` objects, each submitting 3000 events for its tenant (`tenant_id = "load-tenant-" + std::to_string(n)`): 2500 events with `entity_id = "entity-" + std::to_string(n)` (will cross threshold=200) and 500 events with `entity_id = "blocked-" + std::to_string(n)` (on deny list); all events use `event_type="load_test"` and `timestamp=std::chrono::system_clock::now()`; join all threads

- [X] T013 [US2] Add decision collection and P99 assertions in `tests/integration/test_realistic_load_p99.cpp`: `auto result = harness.wait_for_decisions(30'000, 60'000ms)`; `REQUIRE(result.has_value())`; `REQUIRE(result->size() == 30'000u)` (SC-006); collect all `elapsed_us` into a sorted vector; compute `p99_idx = size * 99 / 100`; `INFO("Overall P99: " << p99_us << "µs")`; `REQUIRE(p99_us <= 500'000u)` (FR-007)

- [X] T014 [US2] Add per-tenant P99 and verdict assertions in `tests/integration/test_realistic_load_p99.cpp`: for each of 10 tenants, filter decisions by `tenant_id`, sort `elapsed_us`, assert per-tenant P99 ≤ 500 000µs; assert `std::any_of` finds at least one `Verdict::Flag` decision for that tenant (entity crossed threshold=200); assert `std::any_of` finds at least one `Verdict::Block` decision for that tenant (`blocked-<n>` entity on deny list); add `harness.drain(5000ms)` and `#endif // FRE_ENABLE_DUCKDB` at end of file

**Checkpoint**: `ctest --preset duckdb -R "fre_integration_realistic_load_p99" --output-on-failure` passes; per-tenant P99 INFO lines printed; no DuckDB or deny-list temp files remain after completion.

---

## Phase 4: Polish & Cross-Cutting Concerns

- [X] T015 [P] Build both targets: `cmake --build --preset duckdb 2>&1 | grep -E "(error|warning)"` — confirm zero errors and zero warnings for `fre_unit_realistic_latency_benchmark` and `fre_integration_realistic_load_p99`
- [X] T016 [P] Run full realistic test suite: `ctest --preset duckdb -R "realistic" --output-on-failure` — confirm both tests pass and no temp files remain in `$TMPDIR` matching `fre_bench_*` or `fre_load_*`
- [X] T017 Update `CLAUDE.md` `## Recent Changes` section: add entry `- 007-realistic-perf-tests: Added two DuckDB-backed performance tests with real evaluators (AllowDenyEvaluator + ThresholdEvaluator<WriteBackWindowStore>). test_realistic_latency_benchmark.cpp (unit benchmark, [benchmark][realistic]) and test_realistic_load_p99.cpp (integration load, [integration][load][realistic], P99 ≤ 500ms). Both use on-disk DuckDB temp files with RAII cleanup.`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — T001 and T002 can start immediately and run in parallel
- **US1 (Phase 2)**: Depends on T001 (CMake target must exist before file is created and built); T003–T008 execute sequentially within the same file
- **US2 (Phase 3)**: Depends on T002 (CMake target must exist before file is created and built); T009–T014 execute sequentially within the same file; **US2 can start in parallel with US1** (different files, independent CMake targets)
- **Polish (Phase 4)**: T015 and T016 depend on all of US1 and US2 complete; T017 independent but logically last

### User Story Dependencies

- **US1 (P1)**: Unblocked after T001
- **US2 (P1)**: Unblocked after T002; independent of US1

### Within Each User Story

- Tasks execute sequentially (each builds on the prior task's additions to the same file)
- T003/T009 must precede all subsequent tasks (file must exist)
- T004/T010 must precede T005/T011 (config builder must exist before TEST_CASE body uses it)

---

## Parallel Opportunities

```bash
# Phase 1: Both CMake registrations in parallel
Task T001: tests/unit/CMakeLists.txt
Task T002: tests/integration/CMakeLists.txt

# Phase 2 + Phase 3: Both user story implementations in parallel (after T001/T002)
Task T003-T008: tests/unit/test_realistic_latency_benchmark.cpp
Task T009-T014: tests/integration/test_realistic_load_p99.cpp

# Phase 4: Polish (after both stories complete)
Task T015: cmake --build verify
Task T016: ctest verify
# T017 last (CLAUDE.md update)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: T001 (CMake for US1)
2. Complete Phase 2: T003 → T004 → T005 → T006 → T007 → T008
3. **STOP and VALIDATE**: `ctest --preset duckdb -R "fre_unit_realistic_latency_benchmark" --output-on-failure`
4. Observe benchmark p99 with real evaluators; compare to existing `fre_unit_latency_benchmark` baseline

### Incremental Delivery

1. T001 + T002 → CMake registration ready
2. T003–T008 → US1 benchmark functional and passing
3. T009–T014 → US2 load test functional and passing
4. T015–T017 → Polish and CLAUDE.md updated

### Parallel Team Strategy

- Developer A: T001 then T003–T008 (US1 benchmark)
- Developer B: T002 then T009–T014 (US2 load test)
- Both merge to branch; T015–T017 done together

---

## Notes

- Both test files are entirely self-contained: no shared headers, no new framework code
- `TempFileGuard` is defined locally in each `.cpp` file (copy pattern; no shared header justified for two files)
- The deny-list content for US2 lists all 10 blocked entity IDs in one file (`blocked-0` through `blocked-9`); all 10 tenants share one deny-list file
- The `DuckDbConfig::flush_interval_ms=0` on the `DuckDbWindowStore` disables its own background thread — `WriteBackWindowStore` owns the flush schedule via `WriteBackConfig::flush_interval_ms`
- Post-benchmark assertions in US1 operate on `harness.decisions()` from the final BENCHMARK iteration (last `clear_decisions()` + submit cycle)
- If a test fails mid-run and `harness.drain()` is never called, `TempFileGuard` destructor still runs on scope exit — cleanup is guaranteed
