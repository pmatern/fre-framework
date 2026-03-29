# fre-framework Development Guidelines

Auto-generated from all feature plans. Last updated: 2026-03-26

## Active Technologies
- C++23 (GCC 14+ / Clang 18+) + Asio 1.30.2 (strand dispatch — unchanged), quill 4.5.0 (logging — unchanged), Catch2 3.7.1 (tests — unchanged). No new dependencies. (002-sync-submit-api)
- DuckDB v1.1.3 amalgamation (C API, static lib via CPMAddPackage) — opt-in via `FRE_ENABLE_DUCKDB=ON` / `cmake --preset duckdb`. (004-duckdb-external-storage)
- **Language**: C++23 — GCC 14+ / Clang 18+ (primary); MSVC 19.40 optional
- **Style**: Coroutines (`co_await`/`co_return`), C++23 Concepts, `std::expected<T,E>`, no exceptions, no RTTI
- **Build**: CMake 3.28+ with presets; CPM.cmake for dependency management
- **Coroutine executor**: Standalone Asio 1.30+ (`asio::strand` for shuffle-shard cells)
- **ML inference**: ONNX Runtime 1.19.x (dynamic link, C API at ABI boundaries)
- **Logging**: quill 4.x (lock-free SPSC hot path) + `{fmt}` formatting
- **Testing**: Catch2 v3 (TDD with GIVEN/WHEN/THEN; `BENCHMARK` for latency regression)
- **Serialization**: FlatBuffers (binary protocol); nlohmann/json (REST/debug only)
- **Plugin ABI**: C vtable (`extern "C"` struct of function pointers) + `CppEvaluatorAdapter<Impl>` concept wrapper

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
- 006-write-back-window-store: `WriteBackWindowStore` inverts the DuckDB hot-path relationship — `InProcessWindowStore` is now the authoritative store (< 1ms always); DuckDB is flushed asynchronously in batches by a background `jthread`. Startup recovery seeds in-memory state from DuckDB warm tier. `ThresholdEvaluator<Store>` now a template on `StateStore`. Use `WriteBackWindowStore` instead of `ExternalWindowStore` for new pipelines with DuckDB. `query_range()` flushes dirty entries before querying.
- 005-policy-leaf-nodes: Added 16 new `PolicyRule` leaf node types across 4 groups — tag string matching (`TagContains`, `TagStartsWith`, `TagIn`, `TagExists`), numeric tag comparisons (`TagValueLessThan`, `TagValueGreaterThan`, `TagValueBetween`), first-class Event field matching (`EventTypeIs`, `EventTypeIn`, `TenantIs`, `EventOlderThan`, `EventNewerThan`), and evaluator health/score range (`EvaluatorScoreBetween`, `StageIsDegraded`, `EvaluatorWasSkipped`, `EvaluatorReasonIs`). All nodes compose with `And`/`Or`/`Not`. Numeric parsing uses `strtod` (Apple libc++ has no float `from_chars`). 17 unit test cases + 5 integration scenarios, 103/103 tests passing.
- 004-duckdb-external-storage: Added DuckDB v1.1.3 embedded store satisfying `StateStore` concept. Three-tier hot/warm/cold architecture. `cmake --preset duckdb` to enable. `DuckDbWindowStore` → `as_backend()` → `ExternalWindowStore` — no changes to fallback logic. `WindowedHistoricalEvaluator<Store>` for 30-day lookback. Do NOT call `query_range()` on the hot-path coroutine strand.
- 003-fleet-shuffle-sharding: Added `FleetRouter` (same Vogels 2014 hash as `TenantRouter`). Non-owners return HTTP 503 + `X-Fre-Redirect-Hint`. Seeds in `include/fre/sharding/hash_seeds.hpp` — never change or duplicate. Use non-power-of-2 fleet sizes for strong isolation. Fleet enabled via `HarnessConfig::fleet_config` (optional).

  (ingest → lightweight eval → ML inference → policy eval → decision emit), shuffle-sharded
  multi-tenant isolation, in-process windowed aggregation with optional external state backend.

<!-- MANUAL ADDITIONS START -->
<!-- MANUAL ADDITIONS END -->
