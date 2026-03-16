/// T046 — Integration test: ML inference stage timeout handling.

#include <fre/core/concepts.hpp>
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace fre;
using namespace std::chrono_literals;

// ─── Slow evaluator ───────────────────────────────────────────────────────────

struct SlowEvaluator {
    std::chrono::milliseconds delay;

    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        std::this_thread::sleep_for(delay);
        return EvaluatorResult{
            .evaluator_id = "slow_eval",
            .verdict      = Verdict::Pass,
            .score        = 0.5f,
        };
    }
};
static_assert(InferenceEvaluator<SlowEvaluator>);

// ─── Collecting target ────────────────────────────────────────────────────────

struct TimeoutCollector {
    mutable std::mutex    mu;
    std::vector<Decision> decisions;
    std::atomic<int>      count{0};

    std::expected<void, EmissionError> emit(Decision d) {
        std::lock_guard lock{mu};
        decisions.push_back(std::move(d));
        ++count;
        return {};
    }
};
static_assert(EmissionTarget<TimeoutCollector>);

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ML Inference timeout: degraded decision emitted within latency budget", "[integration][inference_timeout][US4]") {
    auto target = std::make_shared<TimeoutCollector>();

    // Evaluator takes 500ms; stage timeout is 50ms
    auto cfg = PipelineConfig::Builder()
        .pipeline_id("inference-timeout-test")
        .latency_budget(300ms)
        .ingest({})
        .ml_inference(
            InferenceStageConfig{
                .timeout         = 50ms,   // will time out the 500ms evaluator
                .failure_mode    = FailureMode::FailOpen,
                .composition     = CompositionRule::WeightedScore,
                .score_threshold = 0.75f,
            }
            .add_evaluator(SlowEvaluator{.delay = 500ms})
        )
        .emit(EmitStageConfig{.timeout = 10ms}.add_target(target))
        .build();

    REQUIRE(cfg.has_value());
    Pipeline pipeline{std::move(*cfg)};
    REQUIRE(pipeline.start().has_value());

    REQUIRE(pipeline.submit(Event{
        .tenant_id  = "t",
        .entity_id  = "e",
        .event_type = "test",
        .timestamp  = std::chrono::system_clock::now(),
    }).has_value());

    pipeline.drain(3s);

    REQUIRE(target->count.load() == 1);

    std::lock_guard lock{target->mu};
    const auto& d = target->decisions[0];

    // Decision should be emitted (FailOpen)
    REQUIRE(d.event_id != 0);

    // Should be marked degraded due to evaluator timeout
    CHECK(fre::is_degraded(d.degraded_reason));
    CHECK((d.degraded_reason & DegradedReason::EvaluatorTimeout) == DegradedReason::EvaluatorTimeout);

    // FailOpen → verdict should not be Block
    CHECK(d.final_verdict != Verdict::Block);

    // Total elapsed should be within latency budget (300ms)
    INFO("elapsed_us = " << d.elapsed_us);
    CHECK(d.elapsed_us < 300'000);
}
