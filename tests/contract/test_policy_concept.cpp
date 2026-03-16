#include <fre/core/concepts.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>

// ─── Synthetic PolicyEvaluator ────────────────────────────────────────────────

struct SyntheticPolicyEval {
    const fre::PolicyContext* received_ctx{nullptr};
    fre::Verdict              fixed_verdict{fre::Verdict::Pass};

    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::PolicyContext& ctx) {
        received_ctx = &ctx;
        fre::EvaluatorResult r;
        r.evaluator_id = "synthetic_policy";
        r.verdict      = fixed_verdict;
        return r;
    }
};

static_assert(fre::PolicyEvaluator<SyntheticPolicyEval>,
    "SyntheticPolicyEval must satisfy fre::PolicyEvaluator");

// ─── Tests ───────────────────────────────────────────────────────────────────

SCENARIO("PolicyContext contains all preceding stage outputs", "[contract][policy]") {

    GIVEN("a PolicyContext assembled from two stage outputs") {
        // Build a minimal event
        fre::Event event{};
        event.id          = 42;
        event.tenant_id   = "tenant-A";
        event.entity_id   = "entity-X";
        event.event_type  = "test";

        // Simulate two preceding stage outputs
        fre::StageOutput ingest_out;
        ingest_out.stage_id = "ingest";
        ingest_out.verdict  = fre::Verdict::Pass;

        fre::StageOutput eval_out;
        eval_out.stage_id = "eval";
        eval_out.verdict  = fre::Verdict::Flag;

        std::array<fre::StageOutput, 2> outputs{ingest_out, eval_out};

        fre::PolicyContext ctx{
            .event         = event,
            .stage_outputs = std::span<const fre::StageOutput>{outputs},
        };

        WHEN("a PolicyEvaluator::evaluate() is called") {
            SyntheticPolicyEval evaluator;
            auto result = evaluator.evaluate(ctx);

            THEN("the evaluator receives the context") {
                REQUIRE(evaluator.received_ctx == &ctx);
            }

            THEN("stage_outputs contains entries for all preceding stages") {
                REQUIRE(ctx.stage_outputs.size() == 2);
                REQUIRE(ctx.stage_outputs[0].stage_id == "ingest");
                REQUIRE(ctx.stage_outputs[1].stage_id == "eval");
            }

            THEN("context.event reference matches the submitted event") {
                REQUIRE(ctx.event.id == 42);
                REQUIRE(ctx.event.tenant_id == "tenant-A");
                REQUIRE(ctx.event.entity_id == "entity-X");
            }

            THEN("evaluate returns a valid EvaluatorResult") {
                REQUIRE(result.has_value());
                REQUIRE(result->evaluator_id == "synthetic_policy");
                REQUIRE(result->verdict == fre::Verdict::Pass);
            }
        }
    }

    GIVEN("a PolicyEvaluator configured to return Block") {
        fre::Event event{};
        event.id         = 7;
        event.tenant_id  = "t";
        event.entity_id  = "e";
        event.event_type = "suspicious";

        fre::StageOutput eval_out;
        eval_out.stage_id = "eval";
        eval_out.verdict  = fre::Verdict::Flag;

        std::array outputs{eval_out};
        fre::PolicyContext ctx{
            .event         = event,
            .stage_outputs = std::span<const fre::StageOutput>{outputs},
        };

        WHEN("evaluator with Block verdict is invoked") {
            SyntheticPolicyEval evaluator{.fixed_verdict = fre::Verdict::Block};
            auto result = evaluator.evaluate(ctx);

            THEN("result carries Block verdict") {
                REQUIRE(result.has_value());
                REQUIRE(result->verdict == fre::Verdict::Block);
            }
        }
    }
}
