/// T036 — Contract test: ThresholdEvaluator count threshold on 60s window.

#include <fre/core/concepts.hpp>
#include <fre/evaluator/threshold_evaluator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using namespace fre;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("ThresholdEvaluator: 100th event passes, 101st is flagged", "[contract][threshold][US3]") {
    GIVEN("a threshold evaluator with window=60s, count>=100 flags") {
        auto store = std::make_shared<InProcessWindowStore>();
        ThresholdEvaluator evaluator{
            ThresholdEvaluatorConfig{
                .window_duration = 60s,
                .aggregation     = AggregationFn::Count,
                .group_by        = GroupBy::EntityId,
                .threshold       = 100.0,
            },
            store
        };

        const auto now = std::chrono::system_clock::now();

        WHEN("100 events from entity_A are submitted") {
            for (int i = 0; i < 100; ++i) {
                Event ev{
                    .tenant_id  = "acme",
                    .entity_id  = "entity_A",
                    .event_type = "api_call",
                    .timestamp  = now,
                };
                auto result = evaluator.evaluate(ev);
                REQUIRE(result.has_value());
                INFO("Event " << i << " verdict: " << static_cast<int>(result->verdict));

                THEN("event 100 returns Pass") {
                    if (i == 99) {  // 100th event (0-indexed)
                        REQUIRE(result->verdict == Verdict::Pass);
                    }
                }
            }

            THEN("the 101st event returns Flag with reason_code threshold_exceeded") {
                Event ev{
                    .tenant_id  = "acme",
                    .entity_id  = "entity_A",
                    .event_type = "api_call",
                    .timestamp  = now,
                };
                auto result = evaluator.evaluate(ev);
                REQUIRE(result.has_value());
                REQUIRE(result->verdict == Verdict::Flag);
                REQUIRE(result->reason_code.has_value());
                REQUIRE(result->reason_code.value() == "threshold_exceeded");
            }
        }
    }
}
