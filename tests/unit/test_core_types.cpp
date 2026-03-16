#include <fre/core/decision.hpp>
#include <fre/core/error.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <string>

using namespace fre;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Event field accessors", "[core][event]") {
    const std::string tenant = "acme";
    const std::string entity = "user-42";

    Event ev{
        .id         = 7,
        .tenant_id  = tenant,
        .entity_id  = entity,
        .event_type = "api_call",
        .timestamp  = std::chrono::system_clock::now(),
    };

    REQUIRE(ev.id == 7);
    REQUIRE(ev.tenant_id == "acme");
    REQUIRE(ev.entity_id == "user-42");
    REQUIRE(ev.event_type == "api_call");
    REQUIRE(ev.is_valid());
}

TEST_CASE("Event is_valid returns false when required fields empty", "[core][event]") {
    Event ev{};
    REQUIRE_FALSE(ev.is_valid());

    ev.tenant_id = "acme";
    REQUIRE_FALSE(ev.is_valid());  // entity_id still empty

    ev.entity_id = "user-1";
    REQUIRE(ev.is_valid());
}

TEST_CASE("Event tag lookup", "[core][event]") {
    const Tag tags[] = {{"env", "prod"}, {"region", "us-east-1"}};
    Event ev{
        .tenant_id = "t",
        .entity_id = "e",
        .tags      = std::span{tags},
    };

    REQUIRE(ev.tag("env") == "prod");
    REQUIRE(ev.tag("region") == "us-east-1");
    REQUIRE(ev.tag("missing").empty());
}

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Verdict ordering: Block > Flag > Pass", "[core][verdict]") {
    REQUIRE(max_verdict(Verdict::Pass, Verdict::Pass)   == Verdict::Pass);
    REQUIRE(max_verdict(Verdict::Pass, Verdict::Flag)   == Verdict::Flag);
    REQUIRE(max_verdict(Verdict::Flag, Verdict::Block)  == Verdict::Block);
    REQUIRE(max_verdict(Verdict::Block, Verdict::Pass)  == Verdict::Block);
    REQUIRE(max_verdict(Verdict::Flag, Verdict::Flag)   == Verdict::Flag);
}

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("DegradedReason bitmask OR", "[core][verdict]") {
    const DegradedReason none = DegradedReason::None;
    REQUIRE_FALSE(is_degraded(none));

    const DegradedReason a = DegradedReason::EvaluatorTimeout;
    const DegradedReason b = DegradedReason::StateStoreUnavailable;
    const DegradedReason both = a | b;

    REQUIRE(is_degraded(a));
    REQUIRE(is_degraded(b));
    REQUIRE(is_degraded(both));
    REQUIRE((both & a) == a);
    REQUIRE((both & b) == b);

    // compound assignment
    DegradedReason x = DegradedReason::None;
    x |= DegradedReason::EvaluatorError;
    REQUIRE(is_degraded(x));
    REQUIRE((x & DegradedReason::EvaluatorError) == DegradedReason::EvaluatorError);
}

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Decision::compute_final_verdict derives max stage verdict", "[core][decision]") {
    Decision d;
    d.stage_outputs = {
        StageOutput{.stage_id = "ingest",    .verdict = Verdict::Pass},
        StageOutput{.stage_id = "eval",      .verdict = Verdict::Flag},
        StageOutput{.stage_id = "inference", .verdict = Verdict::Pass},
    };
    d.compute_final_verdict();
    REQUIRE(d.final_verdict == Verdict::Flag);

    d.stage_outputs.push_back(StageOutput{.stage_id = "policy", .verdict = Verdict::Block});
    d.compute_final_verdict();
    REQUIRE(d.final_verdict == Verdict::Block);
}

TEST_CASE("Decision::merge_degraded_reasons collects all stage bits", "[core][decision]") {
    Decision d;
    d.stage_outputs = {
        StageOutput{.stage_id = "eval",      .degraded_reason = DegradedReason::EvaluatorTimeout},
        StageOutput{.stage_id = "inference", .degraded_reason = DegradedReason::None},
        StageOutput{.stage_id = "emit",      .degraded_reason = DegradedReason::EmissionRetryExhausted},
    };
    d.merge_degraded_reasons();
    REQUIRE(is_degraded(d.degraded_reason));
    REQUIRE((d.degraded_reason & DegradedReason::EvaluatorTimeout) == DegradedReason::EvaluatorTimeout);
    REQUIRE((d.degraded_reason & DegradedReason::EmissionRetryExhausted) == DegradedReason::EmissionRetryExhausted);
}

TEST_CASE("Decision::stage_output lookup", "[core][decision]") {
    Decision d;
    d.stage_outputs = {
        StageOutput{.stage_id = "eval",  .verdict = Verdict::Flag},
        StageOutput{.stage_id = "ingest", .verdict = Verdict::Pass},
    };

    const auto* eval_out = d.stage_output("eval");
    REQUIRE(eval_out != nullptr);
    REQUIRE(eval_out->verdict == Verdict::Flag);

    REQUIRE(d.stage_output("missing") == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Error::message formats correctly", "[core][error]") {
    GIVEN("a ConfigError with RequiredStageMissing") {
        const ConfigError err{ConfigErrorCode::RequiredStageMissing, "emit"};
        THEN("message contains the code and detail") {
            REQUIRE_FALSE(err.message().empty());
            const std::string msg = err.message();
            CHECK(msg.find("RequiredStageMissing") != std::string::npos);
            CHECK(msg.find("emit") != std::string::npos);
        }
    }

    GIVEN("an EvaluatorError with evaluator_id") {
        const EvaluatorError err{EvaluatorErrorCode::Timeout, "my_eval", "exceeded 200ms"};
        THEN("message includes evaluator_id and detail") {
            const std::string msg = err.message();
            CHECK(msg.find("Timeout") != std::string::npos);
            CHECK(msg.find("my_eval") != std::string::npos);
        }
    }

    GIVEN("a RateLimitError") {
        const RateLimitError err{RateLimitErrorCode::Exhausted, "acme"};
        THEN("message includes tenant_id") {
            const std::string msg = err.message();
            CHECK(msg.find("acme") != std::string::npos);
        }
    }
}
