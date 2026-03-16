# fre-framework

Near Real-Time Detection Pipeline Framework — a C++23 embeddable library for composing event evaluation, ML inference, and policy decisioning pipelines.

## Contents

- [Requirements](#requirements)
- [Building](#building)
- [Running the Examples](#running-the-examples)
- [Running Tests](#running-tests)
- [Quality Gates](#quality-gates)
- [Service Harness](#service-harness)
- [Using as a Library](#using-as-a-library)
- [Pipeline Stages](#pipeline-stages)
- [Configuration Reference](#configuration-reference)
- [CMake Options](#cmake-options)

---

## Requirements

| Tool | Minimum version |
|------|----------------|
| CMake | 3.28 |
| Ninja | any recent |
| GCC | 14+ |
| Clang | 18+ |
| MSVC | 19.40+ (VS 2022 17.10+) |

All library dependencies are fetched automatically by [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) at configure time — no manual installation required:

- Asio 1.30.2 (standalone, header-only)
- quill 4.5.0 (structured logging)
- Catch2 3.7.1 (tests only)
- nlohmann/json 3.11.3 (service harness)
- FlatBuffers 24.3.25 (service harness)

**Optional:**

- ONNX Runtime 1.19+ — required only when `FRE_ENABLE_ONNX=ON`. Must be installed and findable by CMake's `find_package(onnxruntime)`.
- `lcov` + `genhtml` — required for HTML coverage reports (`coverage` preset).
- `clang-tidy` 17+ — required when `FRE_CLANG_TIDY=ON` (used by the `ci` preset).
- `valgrind` — required for `scripts/ci/run-valgrind.sh` (Linux only, advisory).

---

## Building

The project ships with `CMakePresets.json`. All build output lands in `build/<preset>/`.

### Quick start

```bash
# Configure + build in debug mode (includes tests and examples)
cmake --preset debug
cmake --build --preset debug

# Release build (library + service binary only)
cmake --preset release
cmake --build --preset release
```

### Available presets

| Preset | Build type | Tests | Notes |
|--------|-----------|-------|-------|
| `debug` | Debug | ON | Symbols, no optimisation — primary development preset |
| `release` | Release | OFF | `-O3`, LTO |
| `relwithdebinfo` | RelWithDebInfo | ON | Default if no preset given |
| `ci` | RelWithDebInfo | ON | clang-tidy warnings-as-errors (`FRE_CLANG_TIDY=ON`) |
| `coverage` | Debug | ON | `--coverage` instrumentation for lcov/llvm-cov |
| `asan` | Debug | ON | AddressSanitizer + LeakSanitizer |
| `ubsan` | Debug | ON | UndefinedBehaviorSanitizer (`-fno-sanitize-recover=all`) |
| `tsan` | Debug | ON | ThreadSanitizer |

### With ONNX Runtime

```bash
cmake --preset debug -DFRE_ENABLE_ONNX=ON \
      -Donnxruntime_ROOT=/path/to/onnxruntime
cmake --build --preset debug
```

### Without Ninja

```bash
cmake -B build/manual -DCMAKE_BUILD_TYPE=Debug \
      -DFRE_BUILD_TESTS=ON -DFRE_BUILD_EXAMPLES=ON
cmake --build build/manual -j$(nproc)
```

---

## Running the Examples

After building with the `debug` or `relwithdebinfo` preset:

```bash
# Minimal pipeline: custom evaluator + StdoutEmissionTarget
./build/debug/examples/minimal_pipeline/example_minimal_pipeline

# ML inference pipeline: stub anomaly evaluator + PolicyRule + PipelineTestHarness
./build/debug/examples/ml_pipeline/example_ml_pipeline
```

Expected output from the minimal example:

```
[emit] pipeline=minimal-example event=1 verdict=0
[emit] pipeline=minimal-example event=2 verdict=1
[emit] pipeline=minimal-example event=3 verdict=0
```

---

## Running Tests

### All tests

```bash
# Build first
cmake --preset debug && cmake --build --preset debug

# Run all tests
ctest --preset debug
```

### Specific suites

Tests are organised into three subdirectories. Use CTest's `-R` regex filter:

```bash
# Contract tests only (concept satisfaction, interface conformance)
ctest --preset debug -R "fre_contract"

# Unit tests only
ctest --preset debug -R "fre_unit"

# Integration tests only
ctest --preset debug -R "fre_integration"
```

### Individual test binaries

Each test target is a standalone Catch2 executable:

```bash
# Verbose run of one test binary
./build/debug/tests/unit/fre_unit_core_types --reporter console -v high

# Run a single test case by name
./build/debug/tests/integration/fre_integration_minimal_pipeline \
    "10 events produce 10 decisions"

# List all test cases in a binary
./build/debug/tests/unit/fre_unit_eval_stage_composition --list-tests

# Synchronous blocking submit tests
./build/debug/tests/unit/fre_unit_sync_submit --reporter console -v high
./build/debug/tests/integration/fre_integration_sync_submit --reporter console -v high
```

### Benchmarks

The latency benchmark runs as part of the normal test suite with a reduced sample count. To run the full benchmark interactively:

```bash
./build/relwithdebinfo/tests/unit/fre_unit_latency_benchmark \
    "[benchmark]" --reporter console --benchmark-samples 20
```

### Constitution VI gate (sustained load)

Verifies P99 ≤ 300 ms for 10 tenants × 5 000 events. Runs automatically in `ctest` but can be run in isolation:

```bash
./build/relwithdebinfo/tests/integration/fre_integration_load_p99 \
    "[constitution-vi]" --reporter console
```

---

## Quality Gates

Four quality gates are required before merge. CI scripts live in `scripts/ci/`.

### 1. Sanitizer gate (FR-016) — blocking

Builds and runs the full test suite under AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer. All three must pass clean.

```bash
# Run all three sanitizer presets in sequence (exits non-zero on any failure):
./scripts/ci/run-sanitizers.sh

# Or run an individual sanitizer preset manually:
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset ubsan && cmake --build --preset ubsan && ctest --preset ubsan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

Sanitizer logs are written to `build/sanitizer-logs/{asan,ubsan,tsan}.log`.

> **Note**: ASan and TSan are mutually exclusive — never combine them in a single build. UBSan can run alongside ASan; it is kept separate here for cleaner failure attribution.

### 2. Coverage gate (FR-017) — blocking, ≥ 80% line coverage

```bash
# Build with coverage instrumentation and generate the report:
cmake --preset coverage
cmake --build --preset coverage
cmake --build --preset coverage --target coverage

# Enforce the 80% threshold (exits non-zero if below):
./scripts/ci/check-coverage.sh build/coverage/coverage.info

# Check against a custom threshold (e.g. 85%):
./scripts/ci/check-coverage.sh build/coverage/coverage.info 85.0
```

HTML report is generated at `build/coverage/coverage-html/index.html`.

> **Requires**: `lcov` and `genhtml` (`brew install lcov` on macOS; `apt install lcov` on Ubuntu).

### 3. Static analysis gate (FR-018) — blocking

clang-tidy runs on every source file during the `ci` build. Any warning is treated as an error. Suppressions are documented in `.clang-tidy` at the repo root.

```bash
# Build with clang-tidy enabled (requires clang-tidy on PATH):
cmake --preset ci
cmake --build --preset ci
```

To enable clang-tidy on any other preset:

```bash
cmake --preset debug -DFRE_CLANG_TIDY=ON
cmake --build --preset debug
```

### 4. Valgrind advisory gate (FR-019) — non-blocking, Linux only

Runs `valgrind --tool=memcheck` against all test binaries. Always exits 0 — CI archives the report but does not block merge.

```bash
# Requires a debug build first:
cmake --preset debug && cmake --build --preset debug

# Run advisory memcheck:
./scripts/ci/run-valgrind.sh
```

XML reports are written to `build/valgrind/<binary-name>.xml`. Known false positives from quill and asio are suppressed via `valgrind.supp`.

---

## Service Harness

The `fre-service` binary exposes a pipeline over HTTP/1.1 (no Boost dependency). Each request opens a new TCP connection (`Connection: close`).

### Build and run

```bash
# Build
cmake --preset debug
cmake --build --preset debug

# Run with default config (no-op eval, stdout emit, port 8080)
./build/debug/service/fre-service

# Run with a JSON pipeline config file
./build/debug/service/fre-service /path/to/pipeline.json
```

If port 8080 is already in use (e.g. from a previous run):

```bash
lsof -ti :8080 | xargs kill
./build/debug/service/fre-service
```

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/events` | Submit an event; returns `202 Accepted` or `503` if pipeline rejects |
| `GET` | `/health` | Returns pipeline state as JSON |
| `POST` | `/pipeline/drain` | Flushes in-flight events (up to 5 s) and stops the pipeline |

### Submit an event

```bash
curl -s -X POST http://127.0.0.1:8080/events \
  -H 'Content-Type: application/json' \
  -d '{"tenant_id":"acme","entity_id":"user-42","event_type":"api_call"}'
# → HTTP 202 Accepted
```

### Health check

```bash
curl -s http://127.0.0.1:8080/health | jq .
# → {"state":"running","pipeline":"fre-service","degraded":false}
```

Pipeline `state` values: `stopped`, `starting`, `running`, `draining`.

### Drain and stop

```bash
curl -s -X POST http://127.0.0.1:8080/pipeline/drain | jq .
# → {"status":"drained"}
```

---

## Using as a Library

### Via CPM

```cmake
include(cmake/CPM.cmake)

CPMAddPackage(
    NAME fre-framework
    GIT_REPOSITORY https://github.com/your-org/fre-framework
    GIT_TAG        v1.0.0
)

target_link_libraries(my_app PRIVATE fre::pipeline)
```

### Via install + find_package

```bash
cmake --preset release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build --preset release
cmake --install build/release
```

```cmake
find_package(fre 1.0 REQUIRED)
target_link_libraries(my_app PRIVATE fre::pipeline)
```

### Minimal example

```cpp
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>

// A custom evaluator — implement one method, satisfy the concept
struct RateLimitEvaluator {
    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& event) {
        fre::EvaluatorResult r;
        r.evaluator_id = "rate_limit";
        r.verdict      = fre::Verdict::Pass;  // your logic here
        return r;
    }
};
static_assert(fre::LightweightEvaluator<RateLimitEvaluator>);

int main() {
    auto cfg = fre::PipelineConfig::Builder{}
        .pipeline_id("my-pipeline")
        .eval_config(
            fre::EvalStageConfig{}
            .add_evaluator(RateLimitEvaluator{})
        )
        .emit_config(
            fre::EmitStageConfig{}
            .add_stdout_target()
        )
        .build();

    fre::Pipeline pipeline{std::move(*cfg)};
    pipeline.start();

    fre::Event ev{};
    ev.tenant_id  = "acme";
    ev.entity_id  = "user-42";
    ev.event_type = "api_call";
    pipeline.submit(ev);

    pipeline.drain(std::chrono::milliseconds{500});
}
```

### Synchronous (blocking) submit

`submit_sync` submits one event and blocks until the pipeline returns a `Decision` directly — no emission target wiring needed. Emission targets still fire on success.

```cpp
#include <fre/pipeline/sync_submit.hpp>

auto result = pipeline.submit_sync(ev);
if (result) {
    // Decision returned directly
    assert(result->final_verdict == fre::Verdict::Pass);
} else {
    switch (result.error()) {
    case fre::SubmitSyncError::Timeout:            break;
    case fre::SubmitSyncError::RateLimited:        break;
    case fre::SubmitSyncError::PipelineUnavailable: break;
    case fre::SubmitSyncError::NotStarted:         break;
    case fre::SubmitSyncError::ValidationFailed:   break;
    case fre::SubmitSyncError::Cancelled:          break;
    }
}

// With cancellation:
fre::StopSource src;
auto result2 = pipeline.submit_sync(ev, src.get_token());
// From another thread: src.request_stop();
```

### Using the test harness

`PipelineTestHarness` captures decisions synchronously — useful for integration tests and quickstart validation:

```cpp
#include <fre/testing/pipeline_harness.hpp>

fre::testing::PipelineTestHarness harness{std::move(config)};
harness.start();

harness.submit_events(events);
auto decisions = harness.wait_for_decisions(events.size(), 5000ms);
```

---

## Pipeline Stages

All stages are optional except **Ingest** and **Emit**.

```
Event → [Ingest] → [Eval] → [Inference] → [Policy] → [Emit] → Decision
```

| Stage | Header | Purpose |
|-------|--------|---------|
| Ingest | `fre/stage/ingest_stage.hpp` | Validates event fields; enforces clock-skew tolerance |
| Eval | `fre/stage/eval_stage.hpp` | Runs `LightweightEvaluator` implementations; four `CompositionRule` modes |
| Inference | `fre/stage/inference_stage.hpp` | Runs `InferenceEvaluator` implementations with timeout enforcement; `WeightedScore` composition |
| Policy | `fre/stage/policy_stage.hpp` | Evaluates declarative `PolicyRule` AST against accumulated stage outputs |
| Emit | `fre/stage/emit_stage.hpp` | Calls registered `EmissionTarget` implementations; exponential-backoff retry |

### Built-in evaluators

| Evaluator | Header | Concept |
|-----------|--------|---------|
| `ThresholdEvaluator` | `fre/evaluator/threshold_evaluator.hpp` | `LightweightEvaluator` |
| `AllowDenyEvaluator` | `fre/evaluator/allow_deny_evaluator.hpp` | `LightweightEvaluator` |
| `OnnxInferenceEvaluator` | `fre/evaluator/onnx_inference_evaluator.hpp` | `InferenceEvaluator` (requires `FRE_ENABLE_ONNX=ON`) |
| Plugin (shared library) | `fre/evaluator/plugin_loader.hpp` | C vtable ABI (`fre_evaluator_vtable_t`) |

---

## Configuration Reference

### Builder DSL

```cpp
fre::PipelineConfig::Builder{}
    .pipeline_id("name")                    // required
    .pipeline_version("1.0.0")              // default: "1.0.0"
    .latency_budget(300ms)                  // default: 300ms; sum of stage timeouts must fit
    .ingest(fre::IngestStageConfig{...})    // optional; has sensible defaults
    .eval_config(fre::EvalStageConfig{...}) // optional
    .inference_config(...)                  // optional
    .policy_config(...)                     // optional
    .emit_config(...)                       // required — must have ≥1 target
    .rate_limit(fre::RateLimitConfig{...})  // optional; default: 1000 burst, 500 tps, 100 concurrent
    .build()                                // returns std::expected<PipelineConfig, Error>
```

### Rate limit defaults

The default `RateLimitConfig` is conservative (designed for production tenants). Load tests and benchmarks should raise the limits explicitly:

```cpp
fre::RateLimitConfig rate_cfg;
rate_cfg.bucket_capacity   = 100'000;
rate_cfg.tokens_per_second = 200'000;
rate_cfg.max_concurrent    = 10'000;
```

### Failure modes

All stages support `FailureMode`:

| Mode | Effect |
|------|--------|
| `FailOpen` | Treat evaluator failure as `Pass`; mark `skipped=true` |
| `FailClosed` | Treat evaluator failure as `Block`; mark `skipped=true` |
| `EmitDegraded` | Emit the decision with the appropriate `DegradedReason` bit set |

### PolicyRule DSL

```cpp
using namespace fre::policy;

PolicyRule rule = And{
    StageVerdictIs{"eval", fre::Verdict::Flag},
    Or{
        EvaluatorScoreAbove{"anomaly_v1", 0.8f},
        TagEquals{"risk_tier", "high"},
    },
};
```

Node types: `StageVerdictIs`, `EvaluatorScoreAbove`, `TagEquals`, `And`, `Or`, `Not`.

---

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `FRE_BUILD_TESTS` | `ON` | Build all Catch2 test targets |
| `FRE_BUILD_EXAMPLES` | `ON` | Build example binaries |
| `FRE_BUILD_SERVICE` | `ON` | Build `fre-service` HTTP harness |
| `FRE_ENABLE_ONNX` | `OFF` | Enable `OnnxInferenceEvaluator` (requires ONNX Runtime) |
| `FRE_CLANG_TIDY` | `OFF` | Enable clang-tidy with warnings-as-errors during build |
