#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/policy/rule_engine.hpp>
#include <fre/stage/emit_stage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ─── Helpers ──────────────────────────────────────────────────────────────────

namespace {

// Evaluator that always returns Flag
struct FlagEvaluator {
    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& /*event*/) const {
        fre::EvaluatorResult r;
        r.evaluator_id = "flag_eval";
        r.verdict      = fre::Verdict::Flag;
        return r;
    }
};

// Inference evaluator returning a fixed high score
struct HighScoreEval {
    float score;
    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& /*event*/) const {
        fre::EvaluatorResult r;
        r.evaluator_id = "high_score";
        r.verdict      = fre::Verdict::Pass;
        r.score        = score;
        return r;
    }
};

// Capturing emission target
struct CapturingTarget {
    mutable std::mutex              mu;
    std::vector<fre::Decision>      decisions;

    std::expected<void, fre::EmissionError> emit(fre::Decision d) {
        std::lock_guard lock{mu};
        decisions.push_back(std::move(d));
        return {};
    }

    std::size_t count() const {
        std::lock_guard lock{mu};
        return decisions.size();
    }

    fre::Decision get(std::size_t i) const {
        std::lock_guard lock{mu};
        return decisions.at(i);
    }
};

}  // namespace

// ─── Tests ───────────────────────────────────────────────────────────────────

SCENARIO("4-stage pipeline with composite AND policy rule produces Block decision",
         "[integration][policy]")
{
    GIVEN("a 4-stage pipeline: ingest → eval(Flag) → inference(score=0.9) → policy(And rule) → emit")
    {
        using namespace fre::policy;

        auto target = std::make_shared<CapturingTarget>();

        fre::EvalStageConfig eval_cfg;
        eval_cfg.add_evaluator(FlagEvaluator{});

        fre::InferenceStageConfig inf_cfg;
        inf_cfg.add_evaluator(HighScoreEval{.score = 0.9f});
        inf_cfg.score_threshold = 0.0f;  // Don't flag at inference; let policy decide
        inf_cfg.timeout         = 200ms;

        // Policy rule: AND(eval stage = Flag, inference score > 0.8)
        fre::PolicyStageConfig policy_cfg;
        policy_cfg.add_rule(
            PolicyRule{And{
                StageVerdictIs{"eval", fre::Verdict::Flag},
                EvaluatorScoreAbove{"high_score", 0.8f},
            }},
            /*priority=*/ 1,
            /*action_verdict=*/ fre::Verdict::Block,
            /*rule_id=*/ "block_on_high_anomaly");

        fre::EmitStageConfig emit_cfg;
        emit_cfg.add_target(target);

        auto config_result = fre::PipelineConfig::Builder{}
            .pipeline_id("test-policy-pipeline")
            .eval_config(std::move(eval_cfg))
            .inference_config(std::move(inf_cfg))
            .policy_config(std::move(policy_cfg))
            .emit_config(std::move(emit_cfg))
            .build();
        REQUIRE(config_result.has_value());

        fre::Pipeline pipeline{std::move(*config_result)};
        REQUIRE(pipeline.start().has_value());

        WHEN("one event satisfying both AND conditions is submitted") {
            fre::Event event{};
            event.tenant_id  = "tenant-1";
            event.entity_id  = "entity-X";
            event.event_type = "suspicious";

            auto submit_result = pipeline.submit(event);
            REQUIRE(submit_result.has_value());

            pipeline.drain(2000ms);

            THEN("exactly one decision is emitted") {
                REQUIRE(target->count() == 1);
            }

            THEN("final_verdict is Block") {
                auto d = target->get(0);
                REQUIRE(d.final_verdict == fre::Verdict::Block);
            }

            THEN("policy stage output has matched_rule in EvaluatorResult metadata") {
                auto d = target->get(0);
                bool found_policy_stage = false;
                for (const auto& so : d.stage_outputs) {
                    if (so.stage_id == "policy") {
                        found_policy_stage = true;
                        REQUIRE(!so.evaluator_results.empty());
                        bool found_rule_id = false;
                        for (const auto& er : so.evaluator_results) {
                            if (er.reason_code.has_value() &&
                                er.reason_code->find("block_on_high_anomaly") != std::string::npos) {
                                found_rule_id = true;
                            }
                        }
                        REQUIRE(found_rule_id);
                    }
                }
                REQUIRE(found_policy_stage);
            }

            THEN("elapsed_us is under 300ms") {
                auto d = target->get(0);
                REQUIRE(d.elapsed_us < 300'000);
            }
        }
    }
}
