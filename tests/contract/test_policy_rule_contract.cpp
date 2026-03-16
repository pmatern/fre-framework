#include <fre/core/concepts.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>
#include <fre/policy/rule_engine.hpp>

#include <catch2/catch_test_macros.hpp>

// ─── Helpers ──────────────────────────────────────────────────────────────────

static fre::StageOutput make_stage_output(std::string_view stage_id, fre::Verdict v) {
    fre::StageOutput out;
    out.stage_id = std::string{stage_id};
    out.verdict  = v;
    return out;
}

static fre::EvaluatorResult make_eval_result(
    std::string_view evaluator_id, fre::Verdict verdict, float score)
{
    fre::EvaluatorResult r;
    r.evaluator_id = std::string{evaluator_id};
    r.verdict      = verdict;
    r.score        = score;
    return r;
}

// ─── Tests ───────────────────────────────────────────────────────────────────

SCENARIO("PolicyRule And(StageVerdictIs, EvaluatorScoreAbove) matches correctly",
         "[contract][policy]")
{
    GIVEN("a PolicyContext with EvalStage=Flag and an inference evaluator score=0.9") {
        fre::Event event{};
        event.id         = 1;
        event.tenant_id  = "t";
        event.entity_id  = "e";
        event.event_type = "anomaly";

        // Eval stage output: Flag
        fre::StageOutput eval_stage = make_stage_output("eval", fre::Verdict::Flag);
        eval_stage.evaluator_results.push_back(
            make_eval_result("rule_evaluator", fre::Verdict::Flag, 0.0f));

        // Inference stage output: Pass (rule uses raw score)
        fre::StageOutput inf_stage = make_stage_output("inference", fre::Verdict::Pass);
        inf_stage.evaluator_results.push_back(
            make_eval_result("anomaly", fre::Verdict::Pass, 0.9f));

        std::array outputs{eval_stage, inf_stage};
        fre::PolicyContext ctx{
            .event         = event,
            .stage_outputs = std::span<const fre::StageOutput>{outputs},
        };

        WHEN("rule is And(StageVerdictIs(eval, Flag), EvaluatorScoreAbove(anomaly, 0.8))") {
            using namespace fre::policy;

            PolicyRule rule = And{
                StageVerdictIs{"eval", fre::Verdict::Flag},
                EvaluatorScoreAbove{"anomaly", 0.8f},
            };

            THEN("rule matches") {
                auto result = RuleEngine::evaluate(ctx, rule);
                REQUIRE(result == true);
            }
        }

        WHEN("rule is And(StageVerdictIs(eval, Block), EvaluatorScoreAbove(anomaly, 0.8))") {
            using namespace fre::policy;

            PolicyRule rule = And{
                StageVerdictIs{"eval", fre::Verdict::Block},
                EvaluatorScoreAbove{"anomaly", 0.8f},
            };

            THEN("rule does NOT match because eval verdict is Flag not Block") {
                auto result = RuleEngine::evaluate(ctx, rule);
                REQUIRE(result == false);
            }
        }

        WHEN("rule is And(StageVerdictIs(eval, Flag), EvaluatorScoreAbove(anomaly, 0.95))") {
            using namespace fre::policy;

            PolicyRule rule = And{
                StageVerdictIs{"eval", fre::Verdict::Flag},
                EvaluatorScoreAbove{"anomaly", 0.95f},
            };

            THEN("rule does NOT match because score 0.9 < 0.95") {
                auto result = RuleEngine::evaluate(ctx, rule);
                REQUIRE(result == false);
            }
        }
    }

    GIVEN("a PolicyContext with TagEquals rule matching an event tag") {
        fre::Tag  tags[]  = {{"risk_level", "high"}, {"region", "us-west"}};
        fre::Event event{};
        event.id         = 2;
        event.tenant_id  = "t";
        event.entity_id  = "e";
        event.event_type = "risky";
        event.tags       = std::span<const fre::Tag>{tags};

        std::array<fre::StageOutput, 0> outputs{};
        fre::PolicyContext ctx{
            .event         = event,
            .stage_outputs = std::span<const fre::StageOutput>{outputs},
        };

        WHEN("rule is TagEquals(risk_level, high)") {
            using namespace fre::policy;
            PolicyRule rule = TagEquals{"risk_level", "high"};
            THEN("rule matches") {
                REQUIRE(RuleEngine::evaluate(ctx, rule) == true);
            }
        }

        WHEN("rule is TagEquals(risk_level, low)") {
            using namespace fre::policy;
            PolicyRule rule = TagEquals{"risk_level", "low"};
            THEN("rule does NOT match") {
                REQUIRE(RuleEngine::evaluate(ctx, rule) == false);
            }
        }

        WHEN("rule is Or(TagEquals(risk_level, low), TagEquals(region, us-west))") {
            using namespace fre::policy;
            PolicyRule rule = Or{
                TagEquals{"risk_level", "low"},
                TagEquals{"region", "us-west"},
            };
            THEN("rule matches via the second branch") {
                REQUIRE(RuleEngine::evaluate(ctx, rule) == true);
            }
        }

        WHEN("rule is Not(TagEquals(risk_level, low))") {
            using namespace fre::policy;
            PolicyRule rule = Not{TagEquals{"risk_level", "low"}};
            THEN("rule matches because risk_level != low") {
                REQUIRE(RuleEngine::evaluate(ctx, rule) == true);
            }
        }
    }
}
