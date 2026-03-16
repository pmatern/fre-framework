/// T035 — Unit test for EvalStage multi-evaluator verdict composition.
/// Tests all four CompositionRule variants with Flag and Block evaluators.

#include <fre/core/concepts.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/stage/eval_stage.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace fre;
using namespace std::chrono_literals;

// ─── Fixed-verdict evaluators ────────────────────────────────────────────────

struct AlwaysPassEval {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "pass", .verdict = Verdict::Pass};
    }
};

struct AlwaysFlagEval {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "flag", .verdict = Verdict::Flag};
    }
};

struct AlwaysBlockEval {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "block", .verdict = Verdict::Block};
    }
};

static const Event k_test_event{
    .tenant_id  = "t",
    .entity_id  = "e",
    .event_type = "test",
    .timestamp  = std::chrono::system_clock::time_point{},
};

// ─── AnyBlock ────────────────────────────────────────────────────────────────

TEST_CASE("EvalStage AnyBlock: blocks when any evaluator blocks", "[unit][eval_stage][US2]") {
    SECTION("Flag + Block -> Block") {
        EvalStage stage{EvalStageConfig{.composition = CompositionRule::AnyBlock}
            .add_evaluator(AlwaysFlagEval{})
            .add_evaluator(AlwaysBlockEval{})};
        auto out = stage.process(k_test_event);
        REQUIRE(out.has_value());
        REQUIRE(out->verdict == Verdict::Block);
    }

    SECTION("Flag + Flag -> Flag") {
        EvalStage stage{EvalStageConfig{.composition = CompositionRule::AnyBlock}
            .add_evaluator(AlwaysFlagEval{})
            .add_evaluator(AlwaysFlagEval{})};
        auto out = stage.process(k_test_event);
        REQUIRE(out.has_value());
        REQUIRE(out->verdict == Verdict::Flag);
    }

    SECTION("Pass + Pass -> Pass") {
        EvalStage stage{EvalStageConfig{.composition = CompositionRule::AnyBlock}
            .add_evaluator(AlwaysPassEval{})
            .add_evaluator(AlwaysPassEval{})};
        auto out = stage.process(k_test_event);
        REQUIRE(out.has_value());
        REQUIRE(out->verdict == Verdict::Pass);
    }
}

// ─── AnyFlag ─────────────────────────────────────────────────────────────────

TEST_CASE("EvalStage AnyFlag: flags when any evaluator flags", "[unit][eval_stage][US2]") {
    SECTION("Pass + Flag -> Flag") {
        EvalStage stage{EvalStageConfig{.composition = CompositionRule::AnyFlag}
            .add_evaluator(AlwaysPassEval{})
            .add_evaluator(AlwaysFlagEval{})};
        auto out = stage.process(k_test_event);
        REQUIRE(out.has_value());
        REQUIRE(out->verdict == Verdict::Flag);
    }

    SECTION("Flag + Block -> Block") {
        EvalStage stage{EvalStageConfig{.composition = CompositionRule::AnyFlag}
            .add_evaluator(AlwaysFlagEval{})
            .add_evaluator(AlwaysBlockEval{})};
        auto out = stage.process(k_test_event);
        REQUIRE(out.has_value());
        REQUIRE(out->verdict == Verdict::Block);
    }
}

// ─── Unanimous ───────────────────────────────────────────────────────────────

TEST_CASE("EvalStage Unanimous: blocks only if all evaluators block", "[unit][eval_stage][US2]") {
    SECTION("Block + Block -> Block") {
        EvalStage stage{EvalStageConfig{.composition = CompositionRule::Unanimous}
            .add_evaluator(AlwaysBlockEval{})
            .add_evaluator(AlwaysBlockEval{})};
        auto out = stage.process(k_test_event);
        REQUIRE(out.has_value());
        REQUIRE(out->verdict == Verdict::Block);
    }

    SECTION("Flag + Block -> Pass (not all block)") {
        EvalStage stage{EvalStageConfig{.composition = CompositionRule::Unanimous}
            .add_evaluator(AlwaysFlagEval{})
            .add_evaluator(AlwaysBlockEval{})};
        auto out = stage.process(k_test_event);
        REQUIRE(out.has_value());
        REQUIRE(out->verdict == Verdict::Pass);
    }
}

// ─── Majority ────────────────────────────────────────────────────────────────

TEST_CASE("EvalStage Majority: blocks when more than half block", "[unit][eval_stage][US2]") {
    SECTION("2 Block, 1 Flag -> Block (2/3 > half)") {
        EvalStage stage{EvalStageConfig{.composition = CompositionRule::Majority}
            .add_evaluator(AlwaysBlockEval{})
            .add_evaluator(AlwaysBlockEval{})
            .add_evaluator(AlwaysFlagEval{})};
        auto out = stage.process(k_test_event);
        REQUIRE(out.has_value());
        REQUIRE(out->verdict == Verdict::Block);
    }

    SECTION("1 Block, 2 Flag -> Pass (1/3 <= half)") {
        EvalStage stage{EvalStageConfig{.composition = CompositionRule::Majority}
            .add_evaluator(AlwaysBlockEval{})
            .add_evaluator(AlwaysFlagEval{})
            .add_evaluator(AlwaysFlagEval{})};
        auto out = stage.process(k_test_event);
        REQUIRE(out.has_value());
        REQUIRE(out->verdict == Verdict::Pass);
    }

    SECTION("Equal Block + Pass -> Pass (strictly more than half required)") {
        EvalStage stage{EvalStageConfig{.composition = CompositionRule::Majority}
            .add_evaluator(AlwaysBlockEval{})
            .add_evaluator(AlwaysPassEval{})};
        auto out = stage.process(k_test_event);
        REQUIRE(out.has_value());
        REQUIRE(out->verdict == Verdict::Pass);  // 1/2 is not > half
    }
}
