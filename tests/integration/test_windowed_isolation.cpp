/// T037 — Integration test: per-entity windowed isolation and window reset.

#include <fre/core/concepts.hpp>
#include <fre/evaluator/threshold_evaluator.hpp>
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/state/window_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

using namespace fre;
using namespace std::chrono_literals;

// ─── Collecting target ────────────────────────────────────────────────────────

struct VerdictCollector {
    mutable std::mutex    mu;
    std::vector<Decision> decisions;
    std::atomic<int>      count{0};

    std::expected<void, EmissionError> emit(Decision d) {
        std::lock_guard lock{mu};
        decisions.push_back(std::move(d));
        ++count;
        return {};
    }
};
static_assert(EmissionTarget<VerdictCollector>);

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Windowed isolation: entity A exceeds threshold; entity B unaffected", "[integration][windowed][US3]") {
    auto store   = std::make_shared<InProcessWindowStore>();
    auto target  = std::make_shared<VerdictCollector>();

    RateLimitConfig rate_cfg;
    rate_cfg.bucket_capacity   = 10'000;
    rate_cfg.tokens_per_second = 50'000;
    rate_cfg.max_concurrent    = 1'000;

    auto cfg = PipelineConfig::Builder()
        .pipeline_id("windowed-isolation-test")
        .latency_budget(300ms)
        .rate_limit(rate_cfg)
        .ingest({})
        .lightweight_eval(
            EvalStageConfig{
                .timeout      = 50ms,
                .failure_mode = FailureMode::FailOpen,
                .composition  = CompositionRule::AnyBlock,
            }
            .add_evaluator(ThresholdEvaluator{
                ThresholdEvaluatorConfig{
                    .window_duration = 60s,
                    .aggregation     = AggregationFn::Count,
                    .group_by        = GroupBy::EntityId,
                    .threshold       = 100.0,
                },
                store
            })
        )
        .emit(EmitStageConfig{.timeout = 10ms}.add_target(target))
        .build();

    REQUIRE(cfg.has_value());
    Pipeline pipeline{std::move(*cfg)};
    REQUIRE(pipeline.start().has_value());

    const auto now = std::chrono::system_clock::now();

    GIVEN("entity A submits 150 events (crosses threshold at 101)") {
        for (int i = 0; i < 150; ++i) {
            REQUIRE(pipeline.submit(Event{
                .tenant_id  = "acme",
                .entity_id  = "entity-A",
                .event_type = "api_call",
                .timestamp  = now,
            }).has_value());
        }

        GIVEN("entity B submits 50 events") {
            for (int i = 0; i < 50; ++i) {
                REQUIRE(pipeline.submit(Event{
                    .tenant_id  = "acme",
                    .entity_id  = "entity-B",
                    .event_type = "api_call",
                    .timestamp  = now,
                }).has_value());
            }

            pipeline.drain(10s);

            REQUIRE(target->count.load() == 200);

            std::lock_guard lock{target->mu};

            THEN("entity A events 101–150 are flagged") {
                int a_flagged = 0;
                int a_total   = 0;
                for (const auto& d : target->decisions) {
                    if (d.entity_id == "entity-A") {
                        ++a_total;
                        if (d.final_verdict == Verdict::Flag) ++a_flagged;
                    }
                }
                REQUIRE(a_total == 150);
                // At least 50 events should be flagged (101–150)
                REQUIRE(a_flagged >= 50);
            }

            THEN("all entity B events pass") {
                for (const auto& d : target->decisions) {
                    if (d.entity_id == "entity-B") {
                        INFO("entity-B verdict: " << static_cast<int>(d.final_verdict));
                        REQUIRE(d.final_verdict == Verdict::Pass);
                    }
                }
            }
        }
    }
}

TEST_CASE("Windowed isolation: window resets after expiry", "[integration][windowed][US3]") {
    auto store  = std::make_shared<InProcessWindowStore>();
    auto target = std::make_shared<VerdictCollector>();

    const auto short_window = 100ms;

    auto cfg = PipelineConfig::Builder()
        .pipeline_id("window-reset-test")
        .latency_budget(300ms)
        .ingest({})
        .lightweight_eval(
            EvalStageConfig{.timeout = 50ms, .failure_mode = FailureMode::FailOpen}
            .add_evaluator(ThresholdEvaluator{
                ThresholdEvaluatorConfig{
                    .window_duration = short_window,
                    .aggregation     = AggregationFn::Count,
                    .group_by        = GroupBy::EntityId,
                    .threshold       = 5.0,
                },
                store
            })
        )
        .emit(EmitStageConfig{.timeout = 10ms}.add_target(target))
        .build();

    REQUIRE(cfg.has_value());
    Pipeline pipeline{std::move(*cfg)};
    REQUIRE(pipeline.start().has_value());

    // Submit 10 events to exceed threshold in current window
    const auto window1_time = std::chrono::system_clock::now();
    for (int i = 0; i < 10; ++i) {
        REQUIRE(pipeline.submit(Event{
            .tenant_id  = "t",
            .entity_id  = "user-X",
            .event_type = "api_call",
            .timestamp  = window1_time,
        }).has_value());
    }

    // Advance time past window expiry
    const auto window2_time = window1_time + short_window + 1ms;

    // Submit one more event in new window — should start fresh
    REQUIRE(pipeline.submit(Event{
        .tenant_id  = "t",
        .entity_id  = "user-X",
        .event_type = "api_call",
        .timestamp  = window2_time,
    }).has_value());

    pipeline.drain(5s);

    std::lock_guard lock{target->mu};
    REQUIRE_FALSE(target->decisions.empty());
    // Last event (in new window) should not be flagged
    const auto& last = target->decisions.back();
    REQUIRE(last.entity_id == "user-X");
    // Since it's the 1st event in a fresh window (threshold=5), should pass
    REQUIRE(last.final_verdict == Verdict::Pass);
}
