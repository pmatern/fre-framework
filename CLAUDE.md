# fre-framework Development Guidelines

Auto-generated from all feature plans. Last updated: 2026-03-29

## Active Technologies
- C++23 (GCC 14+ / Clang 18+) + Asio 1.30.2 (strand dispatch — unchanged), spdlog 1.14.1 (logging), Catch2 3.7.1 (tests — unchanged). No new dependencies. (002-sync-submit-api)
- DuckDB v1.4.0 amalgamation (C API, static lib via CPMAddPackage) — opt-in via `FRE_ENABLE_DUCKDB=ON` / `cmake --preset duckdb`. (004-duckdb-external-storage)
- **Language**: C++23 — GCC 14+ / Clang 18+ (primary); MSVC 19.40 optional
- **Style**: Coroutines (`co_await`/`co_return`), C++23 Concepts, `std::expected<T,E>`, no exceptions, no RTTI
- **Build**: CMake 3.28+ with presets; CPM.cmake for dependency management
- **Coroutine executor**: Standalone Asio 1.30+ (`asio::strand` for shuffle-shard cells)
- **ML inference**: ONNX Runtime 1.19.x (dynamic link, C API at ABI boundaries)
- **Logging**: spdlog 1.14.1 (async logger, `{fmt}` format strings)
- **Testing**: Catch2 v3 (TDD with GIVEN/WHEN/THEN; `BENCHMARK` for latency regression)
- **Serialization**: FlatBuffers (binary protocol); nlohmann/json (REST/debug only)
- **Plugin ABI**: C vtable (`extern "C"` struct of function pointers) + `CppEvaluatorAdapter<Impl>` concept wrapper
- C++23 — GCC 14+ / Clang 18+ + Catch2 v3.7.1 (benchmark + test macros), DuckDB v1.1.3 (WriteBackWindowStore warm tier), Asio 1.30.2 (pipeline executor) — all pre-existing; no new dependencies (007-realistic-perf-tests)
- On-disk DuckDB via `DuckDbWindowStore` (`DuckDbConfig::db_path` = temp file); cleaned up after each tes (007-realistic-perf-tests)
- On-disk DuckDB via `DuckDbWindowStore` (`DuckDbConfig::db_path` = temp file); deny-list written to second temp file; both cleaned up after each tes (007-realistic-perf-tests)

## Project Structure

```text
include/fre/          # Public headers (installed with library)
  core/               # Event, Verdict, Decision, Error, Concepts
  pipeline/           # Pipeline<Config> + PipelineConfig builder
  stage/              # Per-stage headers (ingest, eval, inference, policy, emit)
  evaluator/          # Built-in evaluators (allow/deny, threshold, ONNX)
  state/              # WindowStore (in-process time-wheel + external store concept)
  policy/             # Rule engine
  sharding/           # TenantRouter (shuffle sharding + token bucket)
src/                  # Implementation units
service/              # Optional standalone service harness (fre-service binary)
tests/
  contract/           # Concept-satisfaction tests + contract conformance
  integration/        # Full pipeline end-to-end tests
  unit/               # Per-component unit tests
examples/             # minimal_pipeline/, ml_pipeline/
cmake/                # FreConfig.cmake.in, dependencies.cmake
```

## Commands

```bash
# Configure (debug)
cmake --preset debug

# Configure (release)
cmake --preset release

# Build
cmake --build --preset debug

# Run tests
ctest --preset debug

# Run tests with verbose output
ctest --preset debug --output-on-failure

# Build service harness
cmake --build --preset release --target fre-service
```

## Code Style

- **Error handling**: All fallible functions return `std::expected<T, E>`. Never throw. Never catch.
- **Async**: Use `asio::awaitable<T>` for coroutines. Wrap returns in `Task<T,E>` for `std::expected` propagation.
- **Concepts**: Define concept satisfaction with `static_assert(ConceptName<MyType>)` alongside the type.
- **Plugins**: Same-binary evaluators satisfy concepts directly. Dynamic-load plugins use the C vtable.
- **Includes**: Use `<fre/...>` paths in all code (mirrors the installed layout).
- **Naming**: `snake_case` for functions and variables; `PascalCase` for types and concepts; `UPPER_SNAKE` for constants.
- **No RTTI**: Do not use `dynamic_cast`, `typeid`, or `std::any`.
- **No raw owning pointers**: Use `std::unique_ptr` / `std::shared_ptr`; prefer value semantics where possible.

## Recent Changes
- 007-realistic-perf-tests: Added two DuckDB-backed performance tests (`FRE_ENABLE_DUCKDB=ON` only): `tests/unit/test_realistic_latency_benchmark.cpp` (Catch2 `BENCHMARK`, P99 measurement with real `AllowDenyEvaluator` + `ThresholdEvaluator<WriteBackWindowStore>` + on-disk DuckDB) and `tests/integration/test_realistic_load_p99.cpp` (10 tenants × 3000 events, P99 ≤ 500ms assertion, per-tenant `Flag`/`Block` verdict checks). Both use PID-based temp file naming and RAII `TempFileGuard` cleanup. The 500ms budget is a conservative CI estimate only — sustained P99 > 300ms in stable conditions is a production constitution violation.
- 006-write-back-window-store: `WriteBackWindowStore` inverts the DuckDB hot-path relationship — `InProcessWindowStore` is now the authoritative store (< 1ms always); DuckDB is flushed asynchronously in batches by a background `jthread`. Startup recovery seeds in-memory state from DuckDB warm tier. `ThresholdEvaluator<Store>` now a template on `StateStore`. Use `WriteBackWindowStore` instead of `ExternalWindowStore` for new pipelines with DuckDB. `query_range()` flushes dirty entries before querying.

  (ingest → lightweight eval → ML inference → policy eval → decision emit), shuffle-sharded
  multi-tenant isolation, in-process windowed aggregation with optional external state backend.

<!-- MANUAL ADDITIONS START -->
<!-- MANUAL ADDITIONS END -->
