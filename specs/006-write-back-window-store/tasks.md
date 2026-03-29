# Tasks: Write-Back Window Store

**Input**: Design documents from `/specs/006-write-back-window-store/`
**Prerequisites**: plan.md ✅, spec.md ✅
**Status**: All tasks completed. Implemented outside speckit on 2026-03-28.

---

## Phase 1: DuckDB Backend Extensions

- [x] T001 Add `upsert_batch(std::span<const std::pair<WindowKey, WindowValue>>)` to `DuckDbWindowStore` — single transaction, unconditional INSERT ON CONFLICT DO UPDATE
- [x] T002 Add `scan_warm_tier() → std::expected<std::vector<std::pair<WindowKey, WindowValue>>, StoreError>` to `DuckDbWindowStore` — SELECT * FROM window_state via query_conn_
- [x] T003 Fix pre-existing typo `duckdb_destroy_prepared` → `duckdb_destroy_prepare` in `StmtHandle`

## Phase 2: ThresholdEvaluator Template

- [x] T004 Templatize `ThresholdEvaluator<Store>` on `StateStore Store`; move implementation inline to header
- [x] T005 Remove `src/evaluator/threshold_evaluator.cpp` from CMakeLists.txt sources (now header-only)
- [x] T006 Verify CTAD: existing call sites `ThresholdEvaluator{config, shared_ptr<InProcessWindowStore>}` compile unchanged

## Phase 3: WriteBackWindowStore

- [x] T007 Create `include/fre/state/write_back_window_store.hpp` — class declaration satisfying `StateStore` and `RangeQueryStore`
- [x] T008 Create `src/state/write_back_window_store.cpp` — hot-path methods, flush_sync(), do_flush(), run_flush_loop(), load_warm_tier()
- [x] T009 Add `src/state/write_back_window_store.cpp` to CMakeLists.txt inside `if(FRE_ENABLE_DUCKDB)`
- [x] T010 Fix pre-existing CMake install/export error: wrap `duckdb::duckdb` in `$<BUILD_INTERFACE:...>`

## Phase 4: Tests

- [x] T011 [P] Write `tests/unit/test_write_back_window_store.cpp` — hot-path isolation, flush, recovery, version chain, expire, DuckDB-unavailable retry, concurrent CAS + flush race
- [x] T012 [P] Write `tests/integration/test_write_back_pipeline.cpp` — functional parity, restart recovery, DuckDB-unavailable resilience
- [x] T013 [P] Register `fre_unit_write_back_window_store` in `tests/unit/CMakeLists.txt`
- [x] T014 [P] Register `fre_integration_write_back_pipeline` in `tests/integration/CMakeLists.txt`

## Phase 5: Concept Checks & Documentation

- [x] T015 Uncomment and expand `static_assert` block in `windowed_historical_evaluator.hpp` for `DuckDbWindowStore` and `WriteBackWindowStore`
- [x] T016 Update `CLAUDE.md` Recent Changes with `006-write-back-window-store` entry

---

## Verification

```bash
cmake --preset duckdb && cmake --build --preset duckdb
ctest --preset duckdb --output-on-failure
# All 103 existing tests pass (debug preset); DuckDB tests require Linux/GCC 14+
```
