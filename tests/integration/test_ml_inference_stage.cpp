/// T045 — Integration test: ML inference stage with stub evaluators.

#include <fre/core/concepts.hpp>
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/stage/inference_stage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

using namespace fre;
using namespace std::chrono_literals;

// ─── Stub evaluators ──────────────────────────────────────────────────────────

struct HighScoreEval {
    float score;
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{
            .evaluator_id = "high_score_eval",
            .verdict      = Verdict::Pass,  // score comparison done by stage
            .score        = score,
        };
    }
};
static_assert(InferenceEvaluator<HighScoreEval>);

// ─── Collecting target ────────────────────────────────────────────────────────

struct InferenceCollector {
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
static_assert(EmissionTarget<InferenceCollector>);

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ML Inference: score above threshold produces Flag verdict", "[integration][inference][US4]") {
    auto target = std::make_shared<InferenceCollector>();

    auto cfg = PipelineConfig::Builder()
        .pipeline_id("ml-inference-above")
        .latency_budget(300ms)
        .ingest({})
        .ml_inference(
            InferenceStageConfig{
                .timeout         = 200ms,
                .failure_mode    = FailureMode::FailOpen,
                .composition     = CompositionRule::WeightedScore,
                .score_threshold = 0.75f,
            }
            .add_evaluator(HighScoreEval{.score = 0.9f})
        )
        .emit(EmitStageConfig{.timeout = 10ms}.add_target(target))
        .build();

    REQUIRE(cfg.has_value());
    Pipeline pipeline{std::move(*cfg)};
    REQUIRE(pipeline.start().has_value());

    for (int i = 0; i < 5; ++i) {
        REQUIRE(pipeline.submit(Event{
            .tenant_id  = "t",
            .entity_id  = "e",
            .event_type = "test",
            .timestamp  = std::chrono::system_clock::now(),
        }).has_value());
    }

    pipeline.drain(5s);
    REQUIRE(target->count.load() == 5);

    std::lock_guard lock{target->mu};
    for (const auto& d : target->decisions) {
        // Score 0.9 > threshold 0.75 → Flag
        const auto* inf_out = d.stage_output("inference");
        REQUIRE(inf_out != nullptr);
        INFO("inference verdict: " << static_cast<int>(inf_out->verdict));
        REQUIRE(inf_out->verdict == Verdict::Flag);

        // EvaluatorResult must record the score
        REQUIRE_FALSE(inf_out->evaluator_results.empty());
        REQUIRE(inf_out->evaluator_results[0].score.has_value());
    }
}

TEST_CASE("ML Inference: score below threshold produces Pass verdict", "[integration][inference][US4]") {
    auto target = std::make_shared<InferenceCollector>();

    auto cfg = PipelineConfig::Builder()
        .pipeline_id("ml-inference-below")
        .latency_budget(300ms)
        .ingest({})
        .ml_inference(
            InferenceStageConfig{
                .timeout         = 200ms,
                .failure_mode    = FailureMode::FailOpen,
                .composition     = CompositionRule::WeightedScore,
                .score_threshold = 0.75f,
            }
            .add_evaluator(HighScoreEval{.score = 0.3f})
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
    const auto* inf_out = target->decisions[0].stage_output("inference");
    REQUIRE(inf_out != nullptr);
    REQUIRE(inf_out->verdict == Verdict::Pass);
}
