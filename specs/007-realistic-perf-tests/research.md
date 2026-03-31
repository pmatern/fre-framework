# Research: Realistic Performance Tests with DuckDB

**Branch**: `007-realistic-perf-tests` | **Date**: 2026-03-29

## Decision 1: Temp file cleanup strategy

**Decision**: Use a local RAII guard struct defined at the top of each test file. The guard holds a `std::filesystem::path` and calls `std::filesystem::remove_all` in its destructor. Constructed at the start of each test case, destroyed (and file deleted) on scope exit — whether the test passes or fails.

**Rationale**: Catch2 does not provide a guaranteed teardown hook that runs on both pass and fail outside of destructors. A local RAII struct is the simplest C++23-idiomatic approach and mirrors how `DuckDbWindowStore` itself cleans up its background thread via `std::jthread`.

**Alternatives considered**:
- Catch2 `SECTION` with cleanup at end: does not run on early `REQUIRE` failure → rejected.
- `std::atexit`: global; interferes with parallel test runs → rejected.
- `std::unique_ptr` with custom deleter: equivalent to RAII guard but less readable → same complexity, no advantage.

---

## Decision 2: Deny list delivery to AllowDenyEvaluator

**Decision**: Write a deny-list temp file at the start of each test using the same `write_list_file` helper pattern from `tests/unit/test_allow_deny_evaluator.cpp`, but scoped to a unique filename per test (e.g., `fre_bench_deny_<pid>.txt`). The RAII guard removes this file alongside the DuckDB file.

**Rationale**: `AllowDenyEvaluator` only accepts a `std::filesystem::path` for its list; there is no in-memory construction path. Writing a small temp file at test startup is the established pattern already in the codebase. The file contains only one line (`"blocked-entity\n"` for the benchmark, one blocked entity per tenant for the load test).

**Alternatives considered**:
- Add an in-memory construction path to `AllowDenyEvaluator`: out of scope; modifies a public class → rejected.
- Skip `AllowDenyEvaluator` and use only `ThresholdEvaluator`: does not satisfy FR-003 → rejected.

---

## Decision 3: DuckDB db_path — on-disk vs. in-memory

**Decision**: Use an on-disk temp file (`std::filesystem::temp_directory_path() / "fre_perf_<pid>.duckdb"`) rather than `db_path = ""` (in-memory mode).

**Rationale**: The spec explicitly requires "write to and read from DuckDB locally" to exercise real persistence. In-memory DuckDB skips the I/O path and does not validate that startup recovery (`scan_warm_tier`) works from a persisted file. On-disk mode is also what production deployments use, so the latency measurement is more representative.

**Alternatives considered**:
- `db_path = ""` (in-memory): existing `test_write_back_pipeline.cpp` already covers this case → redundant; rejected.
- Named temp path without PID: collision risk in parallel ctest runs → rejected.

---

## Decision 4: WriteBackConfig flush_interval_ms

**Decision**: Use `flush_interval_ms = 200` for the benchmark test and `flush_interval_ms = 500` for the load test.

**Rationale**: The background flush thread must fire at least once during each test run to exercise the DuckDB write path, but should not fire so frequently that it inflates p99 with flush I/O on every benchmark iteration. 200ms provides ~5 flushes during a 1000-event benchmark run (estimated 1–2s total). The load test uses 500ms to match the existing `WriteBackConfig` default and minimize flush contention under concurrent load.

**Alternatives considered**:
- `flush_interval_ms = 0`: disables background thread (flushes only at destruction) — does not exercise the background path during the test → rejected.
- `flush_interval_ms = 50`: too frequent for the benchmark; adds measurable flush latency spikes → rejected.

---

## Decision 5: Threshold value and event distribution for meaningful verdicts

**Decision**:
- **Benchmark test** (1000 events total): `threshold = 200`. Submit 500 events for `"high-volume-entity"` (crosses threshold at event 201), 100 events for `"blocked-entity"` (caught by deny list), 400 events for `"normal-entity"` (passes). This guarantees both `Flag` and `Block` verdicts.
- **Load test** (3000 events/tenant): `threshold = 200`. Submit 2500 events for `"entity-<n>"` (crosses threshold) and 500 events for `"blocked-<n>"` (on deny list) per tenant. This guarantees per-tenant `Flag` and `Block` verdicts at scale.

**Rationale**: Threshold must be low enough to be crossed but high enough that the first events pass, demonstrating that the evaluator tracks cumulative state rather than flagging everything. The deny-list entity should represent a small fraction (~10%) of events to keep the overall throughput realistic.

**Alternatives considered**:
- All events for one entity: inflates CAS contention beyond realistic levels → rejected.
- Threshold = 10: crossed too quickly; most events flagged; not representative → rejected.

---

## Decision 6: PID-based temp file uniqueness

**Decision**: Use `std::to_string(getpid())` (POSIX) or a timestamp-based suffix to generate unique temp file names. Since the project targets Linux/macOS (see CLAUDE.md), `getpid()` via `<unistd.h>` is available.

**Rationale**: `ctest` runs tests in parallel by default. Collision between `fre_perf_bench.duckdb` across two simultaneous processes would cause flaky failures.

**Alternatives considered**:
- Random UUID: requires `<random>` with seed; adds complexity for a test helper → rejected.
- Fixed name with `std::filesystem::remove` before creation: still has TOCTOU race under parallel runs → rejected.
