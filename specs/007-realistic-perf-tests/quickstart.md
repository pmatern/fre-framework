# Quickstart: Running the Realistic Performance Tests

**Branch**: `007-realistic-perf-tests`

## Prerequisites

- CMake 3.28+, GCC 14+ or Clang 18+
- No additional system packages needed — DuckDB is bundled via CPM

## Build and run (all realistic perf tests)

```bash
# Configure with DuckDB enabled
cmake --preset duckdb

# Build
cmake --build --preset duckdb

# Run only the new realistic tests
ctest --preset duckdb -R "realistic" --output-on-failure
```

## Run the benchmark only

```bash
ctest --preset duckdb -R "fre_unit_realistic_latency_benchmark" --output-on-failure
```

Catch2 prints benchmark output including mean, p50, p99 (derived from `Decision::elapsed_us`) when run via ctest with `--benchmark-samples 3`.

To run the binary directly for more benchmark samples:

```bash
./build/duckdb/tests/unit/fre_unit_realistic_latency_benchmark \
    "[benchmark][realistic]" \
    --benchmark-samples 10
```

## Run the load test only

```bash
ctest --preset duckdb -R "fre_integration_realistic_load_p99" --output-on-failure
```

The test prints per-tenant P99 values in the `INFO` output and fails if any exceed 500ms.

## Temp file cleanup

Both tests clean up automatically. If a test process is killed mid-run (SIGKILL), temp files may be left at:

```
<system-temp>/fre_perf_<pid>.duckdb
<system-temp>/fre_bench_deny_<pid>.txt
```

These can be safely deleted manually.

## Expected verdicts

| Entity | Evaluator | Expected verdict |
|--------|-----------|-----------------|
| `blocked-entity` (benchmark) / `blocked-<n>` (load) | AllowDenyEvaluator | `Block` |
| `high-volume-entity` (benchmark) / `entity-<n>` after event 201+ | ThresholdEvaluator | `Flag` |
| `normal-entity` (benchmark) / `entity-<n>` under threshold | ThresholdEvaluator | `Pass` |
