#include <fre/core/decision_type.hpp>
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/policy/rule_engine.hpp>
#include <fre/stage/emit_stage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

using namespace std::chrono_literals;

// ─── Helpers ──────────────────────────────────────────────────────────────────

namespace {

struct AlwaysBlockEvaluator {
    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& /*event*/) const {
        fre::EvaluatorResult res;
        res.evaluator_id = "always_block";
        res.verdict      = fre::Verdict::Block;
        return res;
    }
};

struct AlwaysFlagEvaluator {
    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& /*event*/) const {
        fre::EvaluatorResult res;
        res.evaluator_id = "always_flag";
        res.verdict      = fre::Verdict::Flag;
        return res;
    }
};

struct CapturingTarget {
    mutable std::mutex         mu;
    std::vector<fre::Decision> decisions;

    std::expected<void, fre::EmissionError> emit(fre::Decision d) {
        std::lock_guard lock{mu};
        decisions.push_back(std::move(d));
        return {};
    }

    std::size_t count() const {
        std::lock_guard lock{mu};
        return decisions.size();
    }

    fre::Decision get(std::size_t idx) const {
        std::lock_guard lock{mu};
        return decisions.at(idx);
    }
};

fre::Event make_event() {
    fre::Event ev;
    ev.tenant_id  = "tenant-1";
    ev.entity_id  = "entity-1";
    ev.event_type = "login";
    ev.timestamp  = std::chrono::system_clock::now();
    return ev;
}

}  // namespace

// ─── Tests ───────────────────────────────────────────────────────────────────

SCENARIO("Legacy policy (no registry): first-match-wins, single active_decision",
         "[integration][multi_decision]")
{
    GIVEN("a pipeline with two policy rules but no DecisionTypeRegistry")
    {
        using namespace fre::policy;

        auto target = std::make_shared<CapturingTarget>();

        fre::PolicyStageConfig policy_cfg;
        // priority=1 fires first; priority=2 would fire second
        policy_cfg.add_rule(
            fre::policy::PolicyRule{fre::policy::EventTypeIs{"login"}},
            1, fre::Verdict::Flag, "flag_login");
        policy_cfg.add_rule(
            fre::policy::PolicyRule{fre::policy::EventTypeIs{"login"}},
            2, fre::Verdict::Block, "block_login");

        fre::EmitStageConfig emit_cfg;
        emit_cfg.add_target(target);

        auto config = fre::PipelineConfig::Builder{}
            .pipeline_id("legacy-test")
            .policy_config(std::move(policy_cfg))
            .emit(std::move(emit_cfg))
            .build();
        REQUIRE(config.has_value());

        fre::Pipeline pipeline{std::move(*config)};
        REQUIRE(pipeline.start().has_value());

        WHEN("a login event is submitted")
        {
            REQUIRE(pipeline.submit(make_event()).has_value());
            pipeline.drain(2000ms);

            THEN("exactly one decision is emitted")
            {
                REQUIRE(target->count() == 1);
                auto dec = target->get(0);

                AND_THEN("final_verdict is Flag (first match wins)")
                {
                    REQUIRE(dec.final_verdict == fre::Verdict::Flag);
                }

                AND_THEN("active_decisions is empty (no registry configured)")
                {
                    REQUIRE(dec.active_decisions.empty());
                }

                AND_THEN("policy stage has exactly one evaluator result")
                {
                    const auto* pol = dec.stage_output("policy");
                    REQUIRE(pol != nullptr);
                    REQUIRE(pol->evaluator_results.size() == 1);
                    REQUIRE(pol->evaluator_results[0].evaluator_id == "flag_login");
                }
            }
        }
    }
}

SCENARIO("Multi-decision: Block and Notify coexist, Pass+Block conflict resolves to Block",
         "[integration][multi_decision]")
{
    GIVEN("a registry where pass and block are incompatible, notify is compatible with block")
    {
        fre::DecisionTypeRegistry reg;
        REQUIRE(reg.add_type({"block",  "Block",  0  }).has_value());
        REQUIRE(reg.add_type({"notify", "Notify", 50 }).has_value());
        REQUIRE(reg.add_type({"pass",   "Pass",   200}).has_value());
        REQUIRE(reg.add_incompatible("pass", "block").has_value());

        AND_GIVEN("an eval stage that always flags, and three policy rules")
        {
            using namespace fre::policy;

            auto target = std::make_shared<CapturingTarget>();

            fre::EvalStageConfig eval_cfg;
            eval_cfg.add_evaluator(AlwaysFlagEvaluator{});

            fre::PolicyStageConfig policy_cfg;
            // All three rules match the same event (StageVerdictIs Flag is true)
            policy_cfg.add_rule(
                PolicyRule{StageVerdictIs{"eval", fre::Verdict::Flag}},
                1, fre::Verdict::Block,  "block_rule",  "block");
            policy_cfg.add_rule(
                PolicyRule{StageVerdictIs{"eval", fre::Verdict::Flag}},
                2, fre::Verdict::Pass,   "pass_rule",   "pass");
            policy_cfg.add_rule(
                PolicyRule{StageVerdictIs{"eval", fre::Verdict::Flag}},
                3, fre::Verdict::Flag,   "notify_rule", "notify");

            fre::EmitStageConfig emit_cfg;
            emit_cfg.add_target(target);

            auto config = fre::PipelineConfig::Builder{}
                .pipeline_id("multi-decision-test")
                .eval_config(std::move(eval_cfg))
                .policy_config(std::move(policy_cfg))
                .decision_types(std::move(reg))
                .emit(std::move(emit_cfg))
                .build();
            REQUIRE(config.has_value());

            fre::Pipeline pipeline{std::move(*config)};
            REQUIRE(pipeline.start().has_value());

            WHEN("an event is submitted")
            {
                REQUIRE(pipeline.submit(make_event()).has_value());
                pipeline.drain(2000ms);

                THEN("exactly one Decision is emitted")
                {
                    REQUIRE(target->count() == 1);
                    auto dec = target->get(0);

                    AND_THEN("final_verdict is Block (max over all stages)")
                    {
                        REQUIRE(dec.final_verdict == fre::Verdict::Block);
                    }

                    AND_THEN("active_decisions has block and notify but not pass "
                             "(pass was eliminated by incompatibility with block)")
                    {
                        REQUIRE(dec.active_decisions.size() == 2);
                        // Sorted by priority: block(0) first, notify(50) second
                        REQUIRE(dec.active_decisions[0].decision_type_id == "block");
                        REQUIRE(dec.active_decisions[1].decision_type_id == "notify");
                    }

                    AND_THEN("active_decisions does not contain pass")
                    {
                        for (const auto& ad : dec.active_decisions) {
                            REQUIRE(ad.decision_type_id != "pass");
                        }
                    }
                }
            }
        }
    }
}

SCENARIO("Builder::validate rejects a rule with unregistered decision_type_id",
         "[integration][multi_decision]")
{
    GIVEN("a pipeline config with a rule referencing an unregistered type")
    {
        using namespace fre::policy;

        fre::DecisionTypeRegistry reg;
        REQUIRE(reg.add_type({"block", "Block", 0}).has_value());
        // "notify" is NOT registered

        fre::PolicyStageConfig policy_cfg;
        policy_cfg.add_rule(
            PolicyRule{EventTypeIs{"login"}},
            1, fre::Verdict::Flag, "rule1", "notify");  // "notify" unregistered

        fre::EmitStageConfig emit_cfg;
        emit_cfg.add_noop_target();

        WHEN("build() is called")
        {
            auto config = fre::PipelineConfig::Builder{}
                .pipeline_id("bad-config")
                .policy_config(std::move(policy_cfg))
                .decision_types(std::move(reg))
                .emit(std::move(emit_cfg))
                .build();

            THEN("it returns a ConfigError") { REQUIRE_FALSE(config.has_value()); }
        }
    }
}

SCENARIO("Builder::validate rejects a rule with decision_type_id but no registry",
         "[integration][multi_decision]")
{
    GIVEN("a policy rule with a decision_type_id but no registry on the pipeline")
    {
        using namespace fre::policy;

        fre::PolicyStageConfig policy_cfg;
        policy_cfg.add_rule(
            PolicyRule{EventTypeIs{"login"}},
            1, fre::Verdict::Block, "rule1", "block");  // has type id but no registry

        fre::EmitStageConfig emit_cfg;
        emit_cfg.add_noop_target();

        WHEN("build() is called without a decision_types() call")
        {
            auto config = fre::PipelineConfig::Builder{}
                .pipeline_id("no-registry")
                .policy_config(std::move(policy_cfg))
                .emit(std::move(emit_cfg))
                .build();

            THEN("it returns a ConfigError") { REQUIRE_FALSE(config.has_value()); }
        }
    }
}

SCENARIO("Multi-decision: deduplication keeps only the highest-precedence rule per type",
         "[integration][multi_decision]")
{
    GIVEN("two rules emitting the same type, one higher priority than the other")
    {
        using namespace fre::policy;

        fre::DecisionTypeRegistry reg;
        REQUIRE(reg.add_type({"block", "Block", 0}).has_value());

        auto target = std::make_shared<CapturingTarget>();

        fre::PolicyStageConfig policy_cfg;
        // Both match; priority=1 fires first and should win deduplication.
        policy_cfg.add_rule(
            PolicyRule{EventTypeIs{"login"}},
            1, fre::Verdict::Block, "block_high", "block");
        policy_cfg.add_rule(
            PolicyRule{EventTypeIs{"login"}},
            2, fre::Verdict::Block, "block_low", "block");

        fre::EmitStageConfig emit_cfg;
        emit_cfg.add_target(target);

        auto config = fre::PipelineConfig::Builder{}
            .pipeline_id("dedup-test")
            .policy_config(std::move(policy_cfg))
            .decision_types(std::move(reg))
            .emit(std::move(emit_cfg))
            .build();
        REQUIRE(config.has_value());

        fre::Pipeline pipeline{std::move(*config)};
        REQUIRE(pipeline.start().has_value());

        WHEN("a login event is submitted")
        {
            REQUIRE(pipeline.submit(make_event()).has_value());
            pipeline.drain(2000ms);

            THEN("active_decisions has exactly one block entry from the higher-priority rule")
            {
                REQUIRE(target->count() == 1);
                auto dec = target->get(0);
                REQUIRE(dec.active_decisions.size() == 1);
                REQUIRE(dec.active_decisions[0].decision_type_id == "block");
                REQUIRE(dec.active_decisions[0].rule_id == "block_high");
            }
        }
    }
}
