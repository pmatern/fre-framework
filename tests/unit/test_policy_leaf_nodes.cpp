#include <fre/policy/rule_engine.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace fre::policy;
using namespace std::chrono_literals;

// ─── Helpers ──────────────────────────────────────────────────────────────────

namespace {

// Build a minimal Event with a given set of tags.
// Tags are stored in a vector on the stack and the span is set on the event.
struct EventWithTags {
    std::vector<fre::Tag>  tag_storage;
    fre::Event             event;

    explicit EventWithTags(std::initializer_list<std::pair<std::string_view, std::string_view>> tags) {
        for (auto& [k, v] : tags) {
            tag_storage.push_back(fre::Tag{k, v});
        }
        event.tenant_id  = "test-tenant";
        event.entity_id  = "test-entity";
        event.event_type = "test_event";
        event.timestamp  = std::chrono::system_clock::now();
        event.tags       = std::span<const fre::Tag>{tag_storage.data(), tag_storage.size()};
    }
};

// Build a PolicyContext with no stage outputs.
fre::PolicyContext make_ctx(const fre::Event& ev) {
    return fre::PolicyContext{ev, {}};
}

// Build a PolicyContext with provided stage outputs.
fre::PolicyContext make_ctx(const fre::Event& ev,
                            std::span<const fre::StageOutput> outputs) {
    return fre::PolicyContext{ev, outputs};
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// US1: Tag substring and membership matching
// ═══════════════════════════════════════════════════════════════════════════════

SCENARIO("TagContains matches when tag value contains the substring", "[policy][leaf][US1]") {
    GIVEN("a rule TagContains{\"user_agent\", \"bot\"}") {
        PolicyRule rule{TagContains{"user_agent", "bot"}};

        WHEN("event has tag user_agent=crawlerbot/1.0") {
            EventWithTags e{{"user_agent", "crawlerbot/1.0"}};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event has tag user_agent=Firefox/120") {
            EventWithTags e{{"user_agent", "Firefox/120"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("event has no user_agent tag") {
            EventWithTags e{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
    GIVEN("a rule TagContains with empty substring") {
        PolicyRule rule{TagContains{"user_agent", ""}};
        WHEN("event has tag user_agent=anything") {
            EventWithTags e{{"user_agent", "anything"}};
            THEN("empty substring always matches a present tag") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("tag is absent") {
            EventWithTags e{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
}

SCENARIO("TagStartsWith matches when tag value begins with the prefix", "[policy][leaf][US1]") {
    GIVEN("a rule TagStartsWith{\"country\", \"US\"}") {
        PolicyRule rule{TagStartsWith{"country", "US"}};

        WHEN("event has tag country=US-CA") {
            EventWithTags e{{"country", "US-CA"}};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event has tag country=GB") {
            EventWithTags e{{"country", "GB"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("event has no country tag") {
            EventWithTags e{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
    GIVEN("a rule TagStartsWith with empty prefix") {
        PolicyRule rule{TagStartsWith{"country", ""}};
        WHEN("event has tag country=GB") {
            EventWithTags e{{"country", "GB"}};
            THEN("empty prefix always matches a present tag") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
    }
}

SCENARIO("TagIn matches when tag value is in the allowed set", "[policy][leaf][US1]") {
    GIVEN("a rule TagIn{\"risk_tier\", {\"high\",\"critical\"}}") {
        PolicyRule rule{TagIn{"risk_tier", {"high", "critical"}}};

        WHEN("event has tag risk_tier=high") {
            EventWithTags e{{"risk_tier", "high"}};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event has tag risk_tier=critical") {
            EventWithTags e{{"risk_tier", "critical"}};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event has tag risk_tier=low") {
            EventWithTags e{{"risk_tier", "low"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("event has no risk_tier tag") {
            EventWithTags e{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
    GIVEN("a rule TagIn with an empty value set") {
        PolicyRule rule{TagIn{"risk_tier", {}}};
        WHEN("event has tag risk_tier=high") {
            EventWithTags e{{"risk_tier", "high"}};
            THEN("empty set never matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
}

SCENARIO("TagExists matches when the tag is present regardless of value", "[policy][leaf][US1]") {
    GIVEN("a rule TagExists{\"session_id\"}") {
        PolicyRule rule{TagExists{"session_id"}};

        WHEN("event has tag session_id=abc123") {
            EventWithTags e{{"session_id", "abc123"}};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event has tag session_id with empty value") {
            EventWithTags e{{"session_id", ""}};
            THEN("rule matches (value irrelevant)") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event has no session_id tag") {
            EventWithTags e{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
}

SCENARIO("New leaf nodes compose correctly with And and Not (FR-018)", "[policy][leaf][US1][composability]") {
    GIVEN("And{TagContains{ua,bot}, TagExists{session_id}}") {
        PolicyRule rule{And{
            TagContains{"ua", "bot"},
            TagExists{"session_id"}
        }};

        WHEN("both conditions are satisfied") {
            EventWithTags e{{"ua", "crawlerbot"}, {"session_id", "x"}};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("only TagContains is satisfied") {
            EventWithTags e{{"ua", "crawlerbot"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
    GIVEN("Not{TagContains{ua,bot}}") {
        PolicyRule rule{Not{TagContains{"ua", "bot"}}};

        WHEN("event does not have bot in ua") {
            EventWithTags e{{"ua", "Firefox"}};
            THEN("Not inverts: rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event has bot in ua") {
            EventWithTags e{{"ua", "crawlerbot"}};
            THEN("Not inverts: rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// US2: Numeric tag value comparisons
// ═══════════════════════════════════════════════════════════════════════════════

SCENARIO("TagValueGreaterThan matches when parsed value exceeds threshold", "[policy][leaf][US2]") {
    GIVEN("a rule TagValueGreaterThan{\"request_count\", 1000}") {
        PolicyRule rule{TagValueGreaterThan{"request_count", 1000.0}};

        WHEN("tag request_count=1500") {
            EventWithTags e{{"request_count", "1500"}};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("tag request_count=500") {
            EventWithTags e{{"request_count", "500"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("tag request_count=1000 (equal to threshold)") {
            EventWithTags e{{"request_count", "1000"}};
            THEN("strictly greater-than: rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("tag is absent") {
            EventWithTags e{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("tag value is not a number") {
            EventWithTags e{{"request_count", "abc"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
}

SCENARIO("TagValueLessThan matches when parsed value is below threshold", "[policy][leaf][US2]") {
    GIVEN("a rule TagValueLessThan{\"error_rate\", 0.05}") {
        PolicyRule rule{TagValueLessThan{"error_rate", 0.05}};

        WHEN("tag error_rate=0.01") {
            EventWithTags e{{"error_rate", "0.01"}};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("tag error_rate=0.10") {
            EventWithTags e{{"error_rate", "0.10"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("tag error_rate=0.05 (equal to threshold)") {
            EventWithTags e{{"error_rate", "0.05"}};
            THEN("strictly less-than: rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("tag is absent") {
            EventWithTags e{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("tag value is non-numeric") {
            EventWithTags e{{"error_rate", "high"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
}

SCENARIO("TagValueBetween matches when parsed value is in the half-open range lo..hi", "[policy][leaf][US2]") {
    GIVEN("a rule TagValueBetween{\"score\", 0.4, 0.8}") {
        PolicyRule rule{TagValueBetween{"score", 0.4, 0.8}};

        WHEN("tag score=0.6 (in range)") {
            EventWithTags e{{"score", "0.6"}};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("tag score=0.4 (at lower bound, inclusive)") {
            EventWithTags e{{"score", "0.4"}};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("tag score=0.8 (at upper bound, exclusive)") {
            EventWithTags e{{"score", "0.8"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("tag score=0.9 (above range)") {
            EventWithTags e{{"score", "0.9"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("tag score=0.1 (below range)") {
            EventWithTags e{{"score", "0.1"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("tag is absent") {
            EventWithTags e{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("tag value is non-numeric") {
            EventWithTags e{{"score", "n/a"}};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
    GIVEN("a rule TagValueBetween with lo >= hi (vacuously false)") {
        PolicyRule rule{TagValueBetween{"score", 0.8, 0.4}};
        WHEN("tag score=0.6 (would be in range if bounds were sane)") {
            EventWithTags e{{"score", "0.6"}};
            THEN("rule never matches when lo >= hi") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// US3: First-class Event field matching
// ═══════════════════════════════════════════════════════════════════════════════

SCENARIO("EventTypeIs matches on exact event_type", "[policy][leaf][US3]") {
    GIVEN("a rule EventTypeIs{\"login_attempt\"}") {
        PolicyRule rule{EventTypeIs{"login_attempt"}};

        WHEN("event has event_type=login_attempt") {
            EventWithTags e{};
            e.event.event_type = "login_attempt";
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event has event_type=api_call") {
            EventWithTags e{};
            e.event.event_type = "api_call";
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
}

SCENARIO("EventTypeIn matches when event_type is a member of the set", "[policy][leaf][US3]") {
    GIVEN("a rule EventTypeIn{{\"login_attempt\",\"password_reset\"}}") {
        PolicyRule rule{EventTypeIn{{"login_attempt", "password_reset"}}};

        WHEN("event has event_type=password_reset") {
            EventWithTags e{};
            e.event.event_type = "password_reset";
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event has event_type=api_call (not in set)") {
            EventWithTags e{};
            e.event.event_type = "api_call";
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("empty set") {
            PolicyRule empty_rule{EventTypeIn{{}}};
            EventWithTags e{};
            e.event.event_type = "login_attempt";
            THEN("empty set never matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), empty_rule) == false);
            }
        }
    }
}

SCENARIO("TenantIs matches on exact tenant_id", "[policy][leaf][US3]") {
    GIVEN("a rule TenantIs{\"acme\"}") {
        PolicyRule rule{TenantIs{"acme"}};

        WHEN("event has tenant_id=acme") {
            EventWithTags e{};
            e.event.tenant_id = "acme";
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event has tenant_id=globex") {
            EventWithTags e{};
            e.event.tenant_id = "globex";
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
}

SCENARIO("EventOlderThan matches when event age exceeds the duration", "[policy][leaf][US3]") {
    GIVEN("a rule EventOlderThan{30s}") {
        PolicyRule rule{EventOlderThan{std::chrono::seconds{30}}};

        WHEN("event timestamp is 60 seconds in the past") {
            EventWithTags e{};
            e.event.timestamp = std::chrono::system_clock::now() - std::chrono::seconds{60};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event timestamp is 10 seconds in the past") {
            EventWithTags e{};
            e.event.timestamp = std::chrono::system_clock::now() - std::chrono::seconds{10};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("event timestamp is in the future") {
            EventWithTags e{};
            e.event.timestamp = std::chrono::system_clock::now() + std::chrono::seconds{10};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("duration is zero") {
            PolicyRule zero_rule{EventOlderThan{std::chrono::milliseconds{0}}};
            EventWithTags e{};
            e.event.timestamp = std::chrono::system_clock::now() - std::chrono::milliseconds{1};
            THEN("strictly greater-than zero: 1ms old event matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), zero_rule) == true);
            }
        }
    }
}

SCENARIO("EventNewerThan matches when event age is below the duration", "[policy][leaf][US3]") {
    GIVEN("a rule EventNewerThan{5s}") {
        PolicyRule rule{EventNewerThan{std::chrono::seconds{5}}};

        WHEN("event timestamp is 1 second in the past") {
            EventWithTags e{};
            e.event.timestamp = std::chrono::system_clock::now() - std::chrono::seconds{1};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == true);
            }
        }
        WHEN("event timestamp is 10 seconds in the past") {
            EventWithTags e{};
            e.event.timestamp = std::chrono::system_clock::now() - std::chrono::seconds{10};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
        WHEN("event timestamp is in the future") {
            EventWithTags e{};
            e.event.timestamp = std::chrono::system_clock::now() + std::chrono::seconds{10};
            THEN("future timestamp: rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event), rule) == false);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// US4: Evaluator score range and pipeline health predicates
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

// Build a StageOutput with one evaluator result.
fre::StageOutput make_stage(std::string stage_id,
                             fre::Verdict verdict = fre::Verdict::Pass,
                             fre::DegradedReason degraded = fre::DegradedReason::None) {
    fre::StageOutput so;
    so.stage_id        = std::move(stage_id);
    so.verdict         = verdict;
    so.degraded_reason = degraded;
    return so;
}

fre::EvaluatorResult make_eval_result(std::string evaluator_id,
                                       std::optional<float> score = std::nullopt,
                                       bool skipped = false,
                                       std::optional<std::string> reason_code = std::nullopt) {
    fre::EvaluatorResult er;
    er.evaluator_id = std::move(evaluator_id);
    er.score        = score;
    er.skipped      = skipped;
    er.reason_code  = std::move(reason_code);
    return er;
}

}  // namespace

SCENARIO("EvaluatorScoreBetween matches when score is in the half-open range lo..hi", "[policy][leaf][US4]") {
    GIVEN("a rule EvaluatorScoreBetween{\"model\", 0.4f, 0.8f}") {
        PolicyRule rule{EvaluatorScoreBetween{"model", 0.4f, 0.8f}};

        WHEN("evaluator reports score=0.6 (in range)") {
            EventWithTags e{};
            auto so = make_stage("inference");
            so.evaluator_results.push_back(make_eval_result("model", 0.6f));
            std::vector<fre::StageOutput> outputs{so};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == true);
            }
        }
        WHEN("evaluator reports score=0.4 (at lower bound, inclusive)") {
            EventWithTags e{};
            auto so = make_stage("inference");
            so.evaluator_results.push_back(make_eval_result("model", 0.4f));
            std::vector<fre::StageOutput> outputs{so};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == true);
            }
        }
        WHEN("evaluator reports score=0.8 (at upper bound, exclusive)") {
            EventWithTags e{};
            auto so = make_stage("inference");
            so.evaluator_results.push_back(make_eval_result("model", 0.8f));
            std::vector<fre::StageOutput> outputs{so};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == false);
            }
        }
        WHEN("evaluator reports score=0.9 (above range)") {
            EventWithTags e{};
            auto so = make_stage("inference");
            so.evaluator_results.push_back(make_eval_result("model", 0.9f));
            std::vector<fre::StageOutput> outputs{so};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == false);
            }
        }
        WHEN("evaluator did not run") {
            EventWithTags e{};
            std::vector<fre::StageOutput> outputs{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == false);
            }
        }
    }
    GIVEN("a rule EvaluatorScoreBetween with lo >= hi") {
        PolicyRule rule{EvaluatorScoreBetween{"model", 0.8f, 0.4f}};
        WHEN("evaluator reports score=0.6") {
            EventWithTags e{};
            auto so = make_stage("inference");
            so.evaluator_results.push_back(make_eval_result("model", 0.6f));
            std::vector<fre::StageOutput> outputs{so};
            THEN("vacuously false: rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == false);
            }
        }
    }
}

SCENARIO("StageIsDegraded matches when the named stage has a non-zero degraded_reason", "[policy][leaf][US4]") {
    GIVEN("a rule StageIsDegraded{\"inference\"}") {
        PolicyRule rule{StageIsDegraded{"inference"}};

        WHEN("inference stage ran with EvaluatorTimeout degraded reason") {
            EventWithTags e{};
            auto so = make_stage("inference", fre::Verdict::Pass, fre::DegradedReason::EvaluatorTimeout);
            std::vector<fre::StageOutput> outputs{so};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == true);
            }
        }
        WHEN("inference stage ran cleanly") {
            EventWithTags e{};
            auto so = make_stage("inference");
            std::vector<fre::StageOutput> outputs{so};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == false);
            }
        }
        WHEN("inference stage did not run") {
            EventWithTags e{};
            std::vector<fre::StageOutput> outputs{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == false);
            }
        }
    }
}

SCENARIO("EvaluatorWasSkipped matches when the evaluator result has skipped=true", "[policy][leaf][US4]") {
    GIVEN("a rule EvaluatorWasSkipped{\"slow_model\"}") {
        PolicyRule rule{EvaluatorWasSkipped{"slow_model"}};

        WHEN("slow_model result has skipped=true") {
            EventWithTags e{};
            auto so = make_stage("inference");
            so.evaluator_results.push_back(make_eval_result("slow_model", std::nullopt, /*skipped=*/true));
            std::vector<fre::StageOutput> outputs{so};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == true);
            }
        }
        WHEN("slow_model result has skipped=false") {
            EventWithTags e{};
            auto so = make_stage("inference");
            so.evaluator_results.push_back(make_eval_result("slow_model", std::nullopt, /*skipped=*/false));
            std::vector<fre::StageOutput> outputs{so};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == false);
            }
        }
        WHEN("slow_model evaluator did not run") {
            EventWithTags e{};
            std::vector<fre::StageOutput> outputs{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == false);
            }
        }
    }
}

SCENARIO("EvaluatorReasonIs matches when reason_code equals the given string", "[policy][leaf][US4]") {
    GIVEN("a rule EvaluatorReasonIs{\"allow_list\", \"trusted_entity\"}") {
        PolicyRule rule{EvaluatorReasonIs{"allow_list", "trusted_entity"}};

        WHEN("allow_list result has reason_code=trusted_entity") {
            EventWithTags e{};
            auto so = make_stage("eval");
            so.evaluator_results.push_back(
                make_eval_result("allow_list", std::nullopt, false, "trusted_entity"));
            std::vector<fre::StageOutput> outputs{so};
            THEN("rule matches") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == true);
            }
        }
        WHEN("allow_list result has a different reason_code") {
            EventWithTags e{};
            auto so = make_stage("eval");
            so.evaluator_results.push_back(
                make_eval_result("allow_list", std::nullopt, false, "unknown_entity"));
            std::vector<fre::StageOutput> outputs{so};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == false);
            }
        }
        WHEN("allow_list result has no reason_code") {
            EventWithTags e{};
            auto so = make_stage("eval");
            so.evaluator_results.push_back(make_eval_result("allow_list"));
            std::vector<fre::StageOutput> outputs{so};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == false);
            }
        }
        WHEN("allow_list evaluator did not run") {
            EventWithTags e{};
            std::vector<fre::StageOutput> outputs{};
            THEN("rule does not match") {
                CHECK(RuleEngine::evaluate(make_ctx(e.event, outputs), rule) == false);
            }
        }
    }
}
