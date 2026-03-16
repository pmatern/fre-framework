# Quickstart: Near Real-Time Detection Pipeline Framework

**Audience**: Platform engineers integrating the framework into a host application.

## Prerequisites

- CMake 3.28+
- C++23-capable compiler: GCC 14+, Clang 18+, or MSVC 19.40+
- (Optional) ONNX Runtime 1.17+ installed if using the ML inference stage

---

## 1. Add to Your CMake Project

```cmake
# CMakeLists.txt
include(cmake/CPM.cmake)

CPMAddPackage(
  NAME fre-framework
  GIT_REPOSITORY https://github.com/your-org/fre-framework
  GIT_TAG v1.0.0
)

target_link_libraries(my_app PRIVATE fre::pipeline)
```

---

## 2. Minimal Pipeline (Ingest → Lightweight Eval → Emit)

```cpp
#include <fre/pipeline/pipeline.hpp>
#include <fre/evaluator/threshold_evaluator.hpp>

int main() {
    // Build a pipeline that flags entities exceeding 100 events in 60 seconds.
    auto config = fre::PipelineConfig::Builder()
        .pipeline_id("example-pipeline")
        .latency_budget(std::chrono::milliseconds{300})

        .ingest({})

        .lightweight_eval(
            fre::EvalStageConfig{
                .timeout      = std::chrono::milliseconds{10},
                .failure_mode = fre::FailureMode::FailOpen,
                .composition  = fre::CompositionRule::AnyBlock,
            }
            .add_evaluator(fre::ThresholdEvaluator{
                .window_duration = std::chrono::seconds{60},
                .aggregation     = fre::AggregationFn::Count,
                .group_by        = fre::GroupBy::EntityId,
                .threshold       = 100.0,
            })
        )

        .emit(
            fre::EmitStageConfig{
                .timeout      = std::chrono::milliseconds{10},
                .failure_mode = fre::FailureMode::EmitDegraded,
            }
            .add_target(fre::StdoutEmissionTarget{})  // Built-in target for testing
        )

        .build();

    if (!config) {
        std::cerr << "Config error: " << config.error().message() << '\n';
        return 1;
    }

    auto pipeline = fre::Pipeline{std::move(*config)};
    pipeline.start();

    // Submit an event
    fre::Event event{
        .tenant_id  = "acme",
        .entity_id  = "user-42",
        .event_type = "api_call",
        .timestamp  = std::chrono::system_clock::now(),
        .payload    = {},
    };

    auto result = pipeline.submit(std::move(event));
    if (!result) {
        std::cerr << "Submit error: " << result.error().message() << '\n';
    }

    pipeline.drain();
    return 0;
}
```

---

## 3. Plugging In a Custom Evaluator

Implement the `LightweightEvaluator` concept:

```cpp
#include <fre/core/concepts.hpp>

struct MyAllowListEvaluator {
    // The concept requires this exact signature
    auto evaluate(const fre::Event& event)
        -> std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    {
        if (trusted_ids_.contains(event.entity_id)) {
            return fre::EvaluatorResult{
                .evaluator_id = "my_allow_list",
                .verdict      = fre::Verdict::Pass,
            };
        }
        return fre::EvaluatorResult{
            .evaluator_id = "my_allow_list",
            .verdict      = fre::Verdict::Flag,
            .reason_code  = "not_in_allow_list",
        };
    }

private:
    std::unordered_set<std::string_view> trusted_ids_;
};

// Verify concept satisfaction at compile time:
static_assert(fre::LightweightEvaluator<MyAllowListEvaluator>);
```

Register in the pipeline config:

```cpp
.lightweight_eval(
    fre::EvalStageConfig{...}
    .add_evaluator(MyAllowListEvaluator{...})
)
```

---

## 4. Adding ML Inference

```cpp
#include <fre/evaluator/onnx_inference_evaluator.hpp>

.ml_inference(
    fre::InferenceStageConfig{
        .timeout         = std::chrono::milliseconds{200},
        .failure_mode    = fre::FailureMode::FailOpen,
        .composition     = fre::CompositionRule::WeightedScore,
        .score_threshold = 0.75f,
    }
    .add_evaluator(fre::OnnxInferenceEvaluator{
        .model_path      = "models/anomaly_detector.onnx",
        .evaluator_id    = "anomaly_v1",
        .input_extractor = [](const fre::Event& e) { /* extract embedding */ },
    })
)
```

---

## 5. Externalizing Windowed State

To share window state across multiple pipeline instances:

```cpp
#include <fre/state/redis_window_store.hpp>

auto config = fre::PipelineConfig::Builder()
    ...
    .state_store(fre::RedisWindowStoreConfig{
        .host     = "redis.internal",
        .port     = 6379,
        .timeout  = std::chrono::milliseconds{5},
    })
    .build();
```

If the Redis store becomes unavailable, the pipeline automatically falls back to in-process
state and sets `DegradedReason::StateStoreUnavailable` in affected decisions.

---

## 6. Running as a Standalone Service

```bash
# Build the service harness
cmake --preset release
cmake --build --preset release --target fre-service

# Run with a JSON config file
./build/release/fre-service --config pipeline.json --port 8080
```

The service harness exposes:
- `POST /events` — submit one event; returns 202 Accepted (decisions emitted async)
- `GET  /health` — returns pipeline lifecycle state and degradation status
- `POST /pipeline/drain` — graceful shutdown

---

## 7. Validating Your Setup

Run the built-in integration test harness against your pipeline config:

```cpp
#include <fre/testing/pipeline_harness.hpp>

// In your test:
fre::PipelineTestHarness harness{std::move(config)};

harness.submit_events(synthetic_event_batch);
harness.wait_for_decisions(/*timeout=*/std::chrono::seconds{5});

for (const auto& decision : harness.decisions()) {
    REQUIRE(decision.elapsed_us < 300'000);  // P99 budget
    REQUIRE(decision.event_id != 0);
}
```

---

## Common Errors at Startup

| Error | Cause | Fix |
|-------|-------|-----|
| `RequiredStageMissing: emit` | No emission target configured | Add `.emit(...)` to builder |
| `LatencyBudgetExceeded` | Stage timeouts sum > latency_budget | Reduce per-stage timeouts or increase budget |
| `UndefinedStageDependency` | Policy rule references stage not in config | Add the referenced stage or remove the rule |
| `EvaluatorLoadFailed` | ONNX model file not found | Verify model path and file permissions |
