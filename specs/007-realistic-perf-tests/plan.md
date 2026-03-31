# Implementation Plan: Realistic Performance Tests with DuckDB

**Branch**: `007-realistic-perf-tests` | **Date**: 2026-03-29 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/007-realistic-perf-tests/spec.md`

## Summary

Add two test files that replace no-op stubs with real evaluator implementations: `tests/unit/test_realistic_latency_benchmark.cpp` (Catch2 `BENCHMARK`, p50/p99 measurement) and `tests/integration/test_realistic_load_p99.cpp` (10-tenant sustained load, P99 ≤ 500ms assertion). Both use `ThresholdEvaluator<WriteBackWindowStore>` backed by an on-disk DuckDB file, an `AllowDenyEvaluator` with a deny list written to a temp file, a `NoOpInferenceEval` placeholder for the ONNX inference stage, and unconditional RAII cleanup of all temp files at teardown.

## Technical Context

**Language/Version**: C++23 — GCC 14+ / Clang 18+
**Primary Dependencies**: Catch2 v3.7.1 (benchmark + test macros), DuckDB v1.1.3 (WriteBackWindowStore warm tier), Asio 1.30.2 (pipeline executor) — all pre-existing; no new dependencies
**Storage**: On-disk DuckDB via `DuckDbWindowStore` (`DuckDbConfig::db_path` = temp file); deny-list written to second temp file; both cleaned up after each test
**Testing**: Catch2 `BENCHMARK` macro (unit benchmark), `REQUIRE` assertions (integration load test); registered via `if(FRE_ENABLE_DUCKDB)` blocks in `tests/unit/CMakeLists.txt` and `tests/integration/CMakeLists.txt`
**Target Platform**: Linux / macOS local development and CI (same as existing DuckDB tests)
**Project Type**: C++ library — test additions only; no public API surface changes
**Performance Goals**: P99 ≤ 500ms (conservative test-environment budget) for 10 tenants × 3000 events with real evaluators; constitutional 300ms production limit still applies — sustained P99 > 300ms in stable conditions signals a production violation
**Constraints**: Must not regress without `FRE_ENABLE_DUCKDB=ON`; `AllowDenyEvaluator` requires a file-backed deny list; both temp files must not leak on test failure

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Gate | Status | Notes |
|------|--------|-------|
| 1. Spec gate | ✅ PASS | `spec.md` present with user stories and acceptance scenarios |
| 2. Test gate | ✅ PASS | Deliverable IS tests; no implementation code without a corresponding test |
| 3. Dependency gate | ✅ PASS | Zero new dependencies; all evaluators, stores, and test helpers already present |
| 4. Versioning gate | ✅ PASS | No public API changes; test-only additions require no version bump |
| 5. Simplicity gate | ✅ PASS | Two `.cpp` files mirroring existing patterns; no new abstractions beyond a local RAII cleanup guard |
| 6. Resiliency gate | ✅ PASS | These tests ARE the load tests and benchmarks. P99 budget: 500ms test-environment estimate; hot-path P99 documented well under 300ms (O(1) hash + CAS). Failure mode analysis: below. Noisy-tenant isolation: validated by per-tenant P99 assertions across 10 tenants. |

### Resiliency details

**P99 latency budget allocation (load test)**:

| Layer | Budget |
|-------|--------|
| AllowDenyEvaluator (O(1) hash lookup) | < 1ms |
| ThresholdEvaluator hot path (InProcessWindowStore CAS) | < 1ms |
| NoOpInferenceEval stub | < 1ms |
| WriteBackWindowStore flush (background, not on hot path) | async, 0ms added |
| Asio strand dispatch + coroutine overhead | ~1–5ms |
| Rate limiter token bucket | < 1ms |
| Total hot-path P99 (production) | ≤ 300ms (constitutional limit) |
| Total test-environment P99 budget | ≤ 500ms (CI variability + DuckDB I/O) |

**Failure mode analysis**:

| What can fail | Blast radius | Degradation strategy |
|---------------|-------------|---------------------|
| DuckDB file not writable (temp dir full) | Both tests fail at construction | `DuckDbWindowStore::is_available()` returns false; test asserts early with clear message |
| Deny-list file write failure | Test fails at RAII guard construction | `AllowDenyEvaluator` constructor throws `std::runtime_error`; test fails before pipeline starts |
| WriteBackWindowStore flush thread blocked | Background only; hot path unaffected | Dirty entries accumulate and retry; hot-path latency unaffected |
| CAS contention under 10-thread load | Single-entity counters stall up to 10 retries | ThresholdEvaluator returns `Pass` with `reason_code = "cas_contention"` after 10 attempts |

**Noisy-tenant isolation**: Validated by per-tenant P99 assertions. Each tenant gets its own entity IDs and window keys; one tenant's threshold activity cannot inflate another's latency.

## Project Structure

### Documentation (this feature)

```text
specs/007-realistic-perf-tests/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── checklists/
│   └── requirements.md
└── tasks.md             # Phase 2 output (/speckit.tasks)
```

### Source Code (repository root)

```text
tests/
├── unit/
│   ├── CMakeLists.txt                          # add fre_unit_realistic_latency_benchmark block
│   └── test_realistic_latency_benchmark.cpp    # NEW — Catch2 BENCHMARK with real evaluators
└── integration/
    ├── CMakeLists.txt                          # add fre_integration_realistic_load_p99 block
    └── test_realistic_load_p99.cpp             # NEW — 10-tenant load, P99 ≤ 500ms assertion
```

No changes to `include/`, `src/`, or any public header. No changes to `examples/`.

**Structure Decision**: Two new test files, each following the exact file-per-target convention used by every other test in the project. CMake registration follows the `if(FRE_ENABLE_DUCKDB)` block pattern from `tests/unit/CMakeLists.txt:61` and `tests/integration/CMakeLists.txt:79`.
