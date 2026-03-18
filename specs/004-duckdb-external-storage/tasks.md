# Tasks: DuckDB/Parquet External Storage

**Input**: Design documents from `/specs/004-duckdb-external-storage/`
**Status**: ALL TASKS COMPLETE (retroactive documentation — implemented 2026-03-17)

## Phase 1: Setup (Build Infrastructure)

- [x] T001 Add `option(FRE_ENABLE_DUCKDB ...)` and conditional source/link/define block to `CMakeLists.txt`
- [x] T002 Add DuckDB CPMAddPackage block to `cmake/dependencies.cmake` (inside `if(FRE_ENABLE_DUCKDB)`)
- [x] T003 Add `duckdb` configure preset to `CMakePresets.json` (inherits `debug`, sets `FRE_ENABLE_DUCKDB=ON`)
- [x] T004 Add `duckdb-release` configure preset and matching build/test presets to `CMakePresets.json`

**Checkpoint**: `cmake --preset duckdb && cmake --build --preset duckdb` succeeds ✅

---

## Phase 2: Foundational (Core Error Types & Contracts)

- [x] T005 Add `StoreErrorCode::QueryRangeError` to `include/fre/core/error.hpp`
- [x] T006 Implement `StoreErrorCode::QueryRangeError` message case in `src/pipeline/error.cpp`
- [x] T007 Define `DuckDbConfig` struct and `DuckDbWindowStore` class declaration (PIMPL) in `include/fre/state/duckdb_window_store.hpp` (guarded `#ifdef FRE_ENABLE_DUCKDB`)
- [x] T008 Add `static_assert(StateStore<DuckDbWindowStore>)` to header

**Checkpoint**: Header compiles; concept satisfaction asserted ✅

---

## Phase 3: User Story 1 — Warm-Tier CAS State Storage (P1) 🎯 MVP

**Goal**: DuckDB-backed `get`, `compare_and_swap`, `expire`; restart recovery via on-disk WAL.

### Tests for User Story 1

- [x] T009 [P] [US1] Unit test: `is_available()` true for in-memory db — `test_duckdb_window_store.cpp`
- [x] T010 [P] [US1] Unit test: `get()` missing key returns `{0.0, 0}` — `test_duckdb_window_store.cpp`
- [x] T011 [P] [US1] Unit test: CAS from v0 succeeds, version becomes 1 — `test_duckdb_window_store.cpp`
- [x] T012 [P] [US1] Unit test: stale CAS returns false, value unchanged — `test_duckdb_window_store.cpp`
- [x] T013 [P] [US1] Unit test: successive CAS increments version correctly — `test_duckdb_window_store.cpp`
- [x] T014 [P] [US1] Unit test: `expire()` removes row — `test_duckdb_window_store.cpp`
- [x] T015 [P] [US1] Unit test: epoch independence (different epoch = different row) — `test_duckdb_window_store.cpp`
- [x] T016 [US1] Integration test: counting evaluator with DuckDB backend flags correctly — `test_duckdb_backend.cpp`
- [x] T017 [US1] Integration test: restart recovery — second instance reads prior state — `test_duckdb_backend.cpp`

### Implementation for User Story 1

- [x] T018 [US1] Implement RAII wrappers (`DbHandle`, `ConnHandle`, `StmtHandle`, `ResultHandle`) in `duckdb_window_store.cpp`
- [x] T019 [US1] Implement `Impl` constructor: open DB, create schema, prepare statements
- [x] T020 [US1] Implement `get()` using prepared SELECT statement
- [x] T021 [US1] Implement `compare_and_swap()` using BEGIN/INSERT OR IGNORE/UPDATE WHERE version=/COMMIT pattern
- [x] T022 [US1] Implement `expire()` using prepared DELETE statement
- [x] T023 [US1] Implement `is_available()` returning `db_ != nullptr`
- [x] T024 [US1] Implement `as_backend()` filling `ExternalStoreBackend` vtable with lambdas

**Checkpoint**: Warm-tier CAS fully working; restart recovery verified ✅

---

## Phase 4: User Story 2 — Fallback When Database Unavailable (P2)

**Goal**: Bad `db_path` → `is_available()=false` → `ExternalWindowStore` degrades to `InProcessWindowStore`.

### Tests for User Story 2

- [x] T025 [P] [US2] Unit test: `is_available()` false for invalid path — `test_duckdb_window_store.cpp`
- [x] T026 [P] [US2] Unit test: `as_backend()` fills ExternalStoreBackend — `test_duckdb_window_store.cpp`
- [x] T027 [US2] Integration test: bad db_path → fallback activates → `is_degraded()=true` — `test_duckdb_backend.cpp`

*(No implementation tasks — `is_available()` false path is handled by existing `ExternalWindowStore` fallback logic; requires only correct `as_backend()` from T024)*

**Checkpoint**: Fallback path verified end-to-end ✅

---

## Phase 5: User Story 3 — Long-Horizon Range Queries (P3)

**Goal**: `query_range()` sums aggregates across warm (DuckDB) + cold (Parquet) tiers.

### Tests for User Story 3

- [x] T028 [P] [US3] Unit test: `query_range()` sums warm-tier rows — `test_duckdb_window_store.cpp`
- [x] T029 [P] [US3] Unit test: Parquet archive dir configurable — `test_duckdb_window_store.cpp`
- [x] T037 [US3] Integration test: UNION ALL warm+cold path — write values across epochs, set `flush_interval_ms=50`, sleep 100ms to trigger flush, verify Parquet file created, call `query_range()` spanning both tiers, assert combined sum is correct — `test_duckdb_backend.cpp`

### Implementation for User Story 3

- [x] T030 [US3] Implement separate `query_conn` + `query_mutex_` in `Impl`
- [x] T031 [US3] Implement `query_range()` with UNION ALL warm + cold Parquet query
- [x] T032 [US3] Implement background `std::jthread flush_thread_` calling `flush_old_epochs()` every `flush_interval_ms`
- [x] T033 [US3] Implement `flush_old_epochs()`: COPY to Parquet + DELETE old epochs via separate flush connection
- [x] T034 [US3] Create `include/fre/evaluator/windowed_historical_evaluator.hpp` with `RangeQueryStore` concept and `WindowedHistoricalEvaluator<Store>` template

**Checkpoint**: Range queries spanning warm + cold tiers verified ✅

---

## Phase 6: Resiliency Gate — Latency Benchmark

*Required by Constitution Principle VI / Quality Gate 6.*

- [x] T038 `BENCHMARK("DuckDB CAS p99")` in `tests/unit/test_duckdb_window_store.cpp` — target < 10ms; run under `ctest --preset duckdb`

---

## Phase 7: Build Integration & Registration

- [x] T035 [P] Register `fre_unit_duckdb_window_store` test target (requires `FRE_ENABLE_DUCKDB`) in `tests/unit/CMakeLists.txt`
- [x] T036 [P] Register `fre_integration_duckdb_backend` test target in `tests/integration/CMakeLists.txt`

---

## Dependencies & Execution Order

- T001–T004 (build setup) → T005–T008 (foundational types) → T009–T036 (user stories & registration) → T037–T038 (resiliency gate)
- T009–T015 and T025–T026 and T028–T029 (unit tests) can run in parallel once T007 exists
- T016–T017 and T027 (integration tests) depend on T018–T024 (core implementation)
- T030–T034 (range query impl) can proceed in parallel with T025–T027 (fallback tests)
- T035–T036 (CMake registration) require all test files to exist
- T037 (UNION ALL integration test) depends on T030–T034
- T038 (CAS benchmark) depends on T018–T024
