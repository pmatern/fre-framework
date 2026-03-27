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

// Evaluator that returns a configurable score and reason_code.
struct ConfigurableEval {
    std::string  evaluator_id;
    float        score{0.0f};
    bool         skipped{false};
    std::string  reason_code;

    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& /*event*/) const {
        fre::EvaluatorResult r;
        r.evaluator_id = evaluator_id;
        r.verdict      = fre::Verdict::Pass;
        r.score        = score;
        r.skipped      = skipped;
        if (!reason_code.empty()) r.reason_code = reason_code;
        return r;
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

    fre::Decision get(std::size_t i) const {
        std::lock_guard lock{mu};
        return decisions.at(i);
    }
};

// Owns tag storage and exposes a valid fre::Event whose span points into it.
// Must outlive any submit() call since the pipeline accesses the span.
struct OwnedEvent {
    std::vector<fre::Tag> tags;
    fre::Event            event;

    OwnedEvent(std::string tenant_id,
               std::string event_type,
               std::vector<fre::Tag> tag_list = {})
        : tags{std::move(tag_list)} {
        event.tenant_id  = std::move(tenant_id);
        event.event_type = std::move(event_type);
        event.entity_id  = "entity-int-test";
        event.timestamp  = std::chrono::system_clock::now();
        event.tags       = std::span<const fre::Tag>{tags};
    }

    // Non-copyable: span would alias the moved-from storage.
    OwnedEvent(const OwnedEvent&)            = delete;
    OwnedEvent& operator=(const OwnedEvent&) = delete;
    OwnedEvent(OwnedEvent&&)                 = delete;
    OwnedEvent& operator=(OwnedEvent&&)      = delete;
};

// Build and start a minimal pipeline: ingest → eval → policy → emit.
struct PipelineHarness {
    std::shared_ptr<CapturingTarget> target;
    fre::Pipeline                   pipeline;

    explicit PipelineHarness(fre::PolicyStageConfig policy_cfg,
                              ConfigurableEval       evaluator = {})
        : target{std::make_shared<CapturingTarget>()}
        , pipeline{[&] {
            fre::EvalStageConfig eval_cfg;
            if (!evaluator.evaluator_id.empty())
                eval_cfg.add_evaluator(std::move(evaluator));

            fre::EmitStageConfig emit_cfg;
            emit_cfg.add_target(target);

            auto res = fre::PipelineConfig::Builder{}
                .pipeline_id("leaf-node-integration")
                .eval_config(std::move(eval_cfg))
                .policy_config(std::move(policy_cfg))
                .emit_config(std::move(emit_cfg))
                .build();
            REQUIRE(res.has_value());
            return fre::Pipeline{std::move(*res)};
        }()} {
        REQUIRE(pipeline.start().has_value());
    }

    void submit_and_drain(const fre::Event& evt,
                          std::chrono::milliseconds timeout = 500ms) {
        REQUIRE(pipeline.submit(evt).has_value());
        pipeline.drain(timeout);
    }
};

}  // namespace

// ─── Integration tests ────────────────────────────────────────────────────────

SCENARIO("Pipeline with TagContains rule blocks when tag value contains substring",
         "[integration][policy][leaf]")
{
    GIVEN("a policy that blocks when tag 'reason' contains 'fraud'") {
        using namespace fre::policy;

        fre::PolicyStageConfig policy_cfg;
        policy_cfg.add_rule(
            PolicyRule{TagContains{"reason", "fraud"}},
            1, fre::Verdict::Block, "block_on_fraud_tag");

        PipelineHarness h{std::move(policy_cfg)};

        WHEN("an event with reason='card_fraud_detected' is submitted") {
            OwnedEvent ev{"t1", "tx", {{"reason", "card_fraud_detected"}}};
            h.submit_and_drain(ev.event);

            THEN("decision is Block") {
                REQUIRE(h.target->count() == 1);
                REQUIRE(h.target->get(0).final_verdict == fre::Verdict::Block);
            }
        }

        WHEN("an event with reason='chargeback' (no 'fraud') is submitted") {
            OwnedEvent ev{"t1", "tx", {{"reason", "chargeback"}}};
            h.submit_and_drain(ev.event);

            THEN("decision is NOT Block") {
                REQUIRE(h.target->count() == 1);
                REQUIRE(h.target->get(0).final_verdict != fre::Verdict::Block);
            }
        }
    }
}

SCENARIO("Pipeline with TagValueBetween and EventTypeIs composing via And rule",
         "[integration][policy][leaf]")
{
    GIVEN("a policy: And(EventTypeIs('payment'), TagValueBetween('amount', 1000, 10000)) → Flag") {
        using namespace fre::policy;

        fre::PolicyStageConfig policy_cfg;
        policy_cfg.add_rule(
            PolicyRule{And{
                EventTypeIs{"payment"},
                TagValueBetween{"amount", 1000.0, 10000.0},
            }},
            1, fre::Verdict::Flag, "flag_large_payment");

        PipelineHarness h{std::move(policy_cfg)};

        WHEN("a 'payment' event with amount=5000 is submitted") {
            OwnedEvent ev{"t2", "payment", {{"amount", "5000"}}};
            h.submit_and_drain(ev.event);

            THEN("decision is Flag") {
                REQUIRE(h.target->count() == 1);
                REQUIRE(h.target->get(0).final_verdict == fre::Verdict::Flag);
            }
        }

        WHEN("a 'payment' event with amount=500 (below lo) is submitted") {
            OwnedEvent ev{"t2", "payment", {{"amount", "500"}}};
            h.submit_and_drain(ev.event);

            THEN("decision is NOT Flag") {
                REQUIRE(h.target->count() == 1);
                REQUIRE(h.target->get(0).final_verdict != fre::Verdict::Flag);
            }
        }

        WHEN("a 'login' event with amount=5000 is submitted") {
            OwnedEvent ev{"t2", "login", {{"amount", "5000"}}};
            h.submit_and_drain(ev.event);

            THEN("decision is NOT Flag (wrong event type)") {
                REQUIRE(h.target->count() == 1);
                REQUIRE(h.target->get(0).final_verdict != fre::Verdict::Flag);
            }
        }
    }
}

SCENARIO("Pipeline with TenantIs and TagExists composing via And rule",
         "[integration][policy][leaf]")
{
    GIVEN("a policy: And(TenantIs('premium'), TagExists('vip')) → Block") {
        using namespace fre::policy;

        fre::PolicyStageConfig policy_cfg;
        policy_cfg.add_rule(
            PolicyRule{And{
                TenantIs{"premium"},
                TagExists{"vip"},
            }},
            1, fre::Verdict::Block, "block_premium_vip");

        PipelineHarness h{std::move(policy_cfg)};

        WHEN("a premium tenant event with vip tag is submitted") {
            OwnedEvent ev{"premium", "action", {{"vip", "true"}}};
            h.submit_and_drain(ev.event);

            THEN("decision is Block") {
                REQUIRE(h.target->count() == 1);
                REQUIRE(h.target->get(0).final_verdict == fre::Verdict::Block);
            }
        }

        WHEN("a non-premium tenant event with vip tag is submitted") {
            OwnedEvent ev{"basic", "action", {{"vip", "true"}}};
            h.submit_and_drain(ev.event);

            THEN("decision is NOT Block") {
                REQUIRE(h.target->count() == 1);
                REQUIRE(h.target->get(0).final_verdict != fre::Verdict::Block);
            }
        }
    }
}

SCENARIO("Pipeline with TagIn and Not composing via Not(TagIn) rule",
         "[integration][policy][leaf]")
{
    GIVEN("a policy: Not(TagIn('env', {'prod','staging'})) → Flag") {
        using namespace fre::policy;

        fre::PolicyStageConfig policy_cfg;
        policy_cfg.add_rule(
            PolicyRule{Not{TagIn{"env", {"prod", "staging"}}}},
            1, fre::Verdict::Flag, "flag_unknown_env");

        PipelineHarness h{std::move(policy_cfg)};

        WHEN("an event with env='dev' is submitted") {
            OwnedEvent ev{"t3", "deploy", {{"env", "dev"}}};
            h.submit_and_drain(ev.event);

            THEN("decision is Flag") {
                REQUIRE(h.target->count() == 1);
                REQUIRE(h.target->get(0).final_verdict == fre::Verdict::Flag);
            }
        }

        WHEN("an event with env='prod' is submitted") {
            OwnedEvent ev{"t3", "deploy", {{"env", "prod"}}};
            h.submit_and_drain(ev.event);

            THEN("decision is NOT Flag") {
                REQUIRE(h.target->count() == 1);
                REQUIRE(h.target->get(0).final_verdict != fre::Verdict::Flag);
            }
        }
    }
}

SCENARIO("Pipeline with EvaluatorReasonIs rule blocks on specific reason code",
         "[integration][policy][leaf]")
{
    GIVEN("an evaluator returning reason_code='high_velocity' and a matching policy rule") {
        using namespace fre::policy;

        fre::PolicyStageConfig policy_cfg;
        policy_cfg.add_rule(
            PolicyRule{EvaluatorReasonIs{"risk_eval", "high_velocity"}},
            1, fre::Verdict::Block, "block_high_velocity");

        ConfigurableEval eval;
        eval.evaluator_id = "risk_eval";
        eval.score        = 0.5f;
        eval.reason_code  = "high_velocity";

        PipelineHarness h{std::move(policy_cfg), std::move(eval)};

        WHEN("an event is submitted") {
            OwnedEvent ev{"t4", "transfer"};
            h.submit_and_drain(ev.event);

            THEN("decision is Block") {
                REQUIRE(h.target->count() == 1);
                REQUIRE(h.target->get(0).final_verdict == fre::Verdict::Block);
            }
        }
    }
}
