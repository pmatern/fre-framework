/// T028 — Contract test: multi-evaluator composition with custom evaluators.

#include <fre/core/concepts.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/stage/eval_stage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace fre;
using namespace std::chrono_literals;

// ─── Synthetic evaluators ─────────────────────────────────────────────────────

struct SyntheticFlagEvaluator {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "flag_eval", .verdict = Verdict::Flag};
    }
};
static_assert(LightweightEvaluator<SyntheticFlagEvaluator>);

struct SyntheticBlockEvaluator {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "block_eval", .verdict = Verdict::Block};
    }
};
static_assert(LightweightEvaluator<SyntheticBlockEvaluator>);

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Custom evaluators: AnyBlock composition blocks when any blocks", "[contract][custom_eval][US2]") {
    GIVEN("two evaluators — one Flag, one Block — with AnyBlock composition") {
        EvalStage stage{
            EvalStageConfig{
                .timeout      = 10ms,
                .failure_mode = FailureMode::FailOpen,
                .composition  = CompositionRule::AnyBlock,
            }
            .add_evaluator(SyntheticFlagEvaluator{})
            .add_evaluator(SyntheticBlockEvaluator{})
        };

        Event ev{
            .tenant_id  = "t",
            .entity_id  = "e",
            .event_type = "test",
            .timestamp  = std::chrono::system_clock::now(),
        };

        WHEN("an event is evaluated") {
            auto out = stage.process(ev);

            THEN("StageOutput verdict is Block") {
                REQUIRE(out.has_value());
                REQUIRE(out->verdict == Verdict::Block);
            }

            THEN("both EvaluatorResult entries are present in stage_outputs") {
                REQUIRE(out.has_value());
                REQUIRE(out->evaluator_results.size() == 2);
                bool found_flag  = false;
                bool found_block = false;
                for (const auto& r : out->evaluator_results) {
                    if (r.evaluator_id == "flag_eval")  found_flag  = true;
                    if (r.evaluator_id == "block_eval") found_block = true;
                }
                CHECK(found_flag);
                CHECK(found_block);
            }
        }
    }
}

TEST_CASE("Custom evaluators: two Flag evaluators with Unanimous yields Pass", "[contract][custom_eval][US2]") {
    GIVEN("two Flag evaluators with Unanimous composition") {
        EvalStage stage{
            EvalStageConfig{
                .composition = CompositionRule::Unanimous,
            }
            .add_evaluator(SyntheticFlagEvaluator{})
            .add_evaluator(SyntheticFlagEvaluator{})
        };

        Event ev{
            .tenant_id  = "t",
            .entity_id  = "e",
            .event_type = "test",
            .timestamp  = std::chrono::system_clock::now(),
        };

        auto out = stage.process(ev);
        REQUIRE(out.has_value());

        // Unanimous: Block only if ALL block — Flag evaluators do not trigger Block
        CHECK(out->verdict != Verdict::Block);
    }
}
