#include <fre/core/concepts.hpp>
#include <fre/core/event.hpp>
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

using namespace fre;
using namespace std::chrono_literals;

// ─── Synthetic pass evaluator ─────────────────────────────────────────────────

struct SyntheticPassEvaluator {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "synthetic_pass", .verdict = Verdict::Pass};
    }
};
static_assert(LightweightEvaluator<SyntheticPassEvaluator>);

// ─── Collecting target ────────────────────────────────────────────────────────

struct SyncCollectingTarget {
    mutable std::mutex       mu;
    std::vector<Decision>    decisions;
    std::atomic<int>         count{0};

    std::expected<void, EmissionError> emit(Decision d) {
        {
            std::lock_guard lock{mu};
            decisions.push_back(std::move(d));
        }
        ++count;
        return {};
    }
};
static_assert(EmissionTarget<SyncCollectingTarget>);

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Minimal pipeline: 10 events produce 10 decisions", "[integration][us1]") {
    GIVEN("a 3-stage pipeline with a pass-all evaluator") {
        auto target = std::make_shared<SyncCollectingTarget>();

        auto cfg_result = PipelineConfig::Builder()
            .pipeline_id("minimal-e2e")
            .latency_budget(300ms)
            .ingest({})
            .lightweight_eval(
                EvalStageConfig{
                    .timeout      = 10ms,
                    .failure_mode = FailureMode::FailOpen,
                    .composition  = CompositionRule::AnyBlock,
                }
                .add_evaluator(SyntheticPassEvaluator{})
            )
            .emit(
                EmitStageConfig{
                    .timeout      = 10ms,
                    .failure_mode = FailureMode::EmitDegraded,
                }
                .add_target(target)
            )
            .build();

        REQUIRE(cfg_result.has_value());

        Pipeline pipeline{std::move(*cfg_result)};
        REQUIRE(pipeline.start().has_value());

        WHEN("10 events are submitted") {
            for (int i = 0; i < 10; ++i) {
                Event ev{
                    .tenant_id  = "acme",
                    .entity_id  = std::string{"user-"} + std::to_string(i),
                    .event_type = "api_call",
                    .timestamp  = std::chrono::system_clock::now(),
                };
                auto result = pipeline.submit(std::move(ev));
                REQUIRE(result.has_value());
            }

            pipeline.drain(5s);

            THEN("exactly 10 decisions are emitted") {
                REQUIRE(target->count.load() == 10);
            }

            THEN("all decisions have final_verdict = Pass") {
                std::lock_guard lock{target->mu};
                for (const auto& d : target->decisions) {
                    REQUIRE(d.final_verdict == Verdict::Pass);
                }
            }

            THEN("all decisions have elapsed_us within the latency budget") {
                std::lock_guard lock{target->mu};
                for (const auto& d : target->decisions) {
                    INFO("elapsed_us = " << d.elapsed_us);
                    REQUIRE(d.elapsed_us < 300'000);
                }
            }

            THEN("all decisions carry stage_outputs for ingest and eval stages") {
                std::lock_guard lock{target->mu};
                for (const auto& d : target->decisions) {
                    // Ingest + Eval = 2 stage outputs (emit stage doesn't add one)
                    REQUIRE(d.stage_outputs.size() == 2);
                }
            }
        }
    }
}

TEST_CASE("Minimal pipeline: invalid event (empty entity_id) triggers degraded decision", "[integration][us1]") {
    auto target = std::make_shared<SyncCollectingTarget>();

    auto cfg_result = PipelineConfig::Builder()
        .pipeline_id("ingest-validation-test")
        .latency_budget(300ms)
        .ingest({})
        .emit(
            EmitStageConfig{
                .timeout      = 10ms,
                .failure_mode = FailureMode::EmitDegraded,
            }
            .add_target(target)
        )
        .build();
    REQUIRE(cfg_result.has_value());

    Pipeline pipeline{std::move(*cfg_result)};
    REQUIRE(pipeline.start().has_value());

    Event invalid_ev{
        .tenant_id  = "acme",
        .entity_id  = "",          // deliberately invalid
        .event_type = "api_call",
        .timestamp  = std::chrono::system_clock::now(),
    };
    auto result = pipeline.submit(std::move(invalid_ev));

    // Either submit rejects the event, or it emits a degraded decision
    if (result.has_value()) {
        pipeline.drain(2s);
        REQUIRE(target->count.load() == 1);
        std::lock_guard lock{target->mu};
        REQUIRE(fre::is_degraded(target->decisions[0].degraded_reason));
    } else {
        // Submit-time validation — acceptable
        REQUIRE_FALSE(result.has_value());
    }
}
