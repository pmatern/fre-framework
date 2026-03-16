/// T029 — Integration test: custom evaluator plug-in and failure mode handling.

#include <fre/core/concepts.hpp>
#include <fre/core/error.hpp>
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

using namespace fre;
using namespace std::chrono_literals;

// ─── Synthetic evaluators ─────────────────────────────────────────────────────

struct BlockAllEvaluator {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{
            .evaluator_id = "block_all",
            .verdict      = Verdict::Block,
            .reason_code  = "always_block",
        };
    }
};
static_assert(LightweightEvaluator<BlockAllEvaluator>);

struct ErroringEvaluator {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return std::unexpected(EvaluatorError{
            .code         = EvaluatorErrorCode::InternalError,
            .evaluator_id = "erroring_eval",
            .detail       = "deliberate error for test",
        });
    }
};
static_assert(LightweightEvaluator<ErroringEvaluator>);

// ─── Collecting target ────────────────────────────────────────────────────────

struct CollectTarget {
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
static_assert(EmissionTarget<CollectTarget>);

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Custom evaluator: BlockAll produces Block verdict for all events", "[integration][custom_eval][US2]") {
    auto target = std::make_shared<CollectTarget>();

    auto cfg = PipelineConfig::Builder()
        .pipeline_id("block-all-test")
        .latency_budget(300ms)
        .ingest({})
        .lightweight_eval(
            EvalStageConfig{.timeout = 10ms, .failure_mode = FailureMode::FailOpen, .composition = CompositionRule::AnyBlock}
            .add_evaluator(BlockAllEvaluator{})
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
        REQUIRE(d.final_verdict == Verdict::Block);
    }
}

TEST_CASE("Custom evaluator: ErroringEvaluator with FailOpen produces degraded decision without crash", "[integration][custom_eval][US2]") {
    auto target = std::make_shared<CollectTarget>();

    auto cfg = PipelineConfig::Builder()
        .pipeline_id("erroring-eval-test")
        .latency_budget(300ms)
        .ingest({})
        .lightweight_eval(
            EvalStageConfig{.timeout = 10ms, .failure_mode = FailureMode::FailOpen}
            .add_evaluator(ErroringEvaluator{})
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

    // Should have emitted a decision (fail-open) with degradation indicator
    REQUIRE(target->count.load() == 1);
    std::lock_guard lock{target->mu};
    const auto& d = target->decisions[0];
    CHECK(fre::is_degraded(d.degraded_reason));
    // FailOpen means the stage verdict is Pass (not Block)
    CHECK(d.final_verdict != Verdict::Block);
}
