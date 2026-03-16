#include <fre/core/concepts.hpp>
#include <fre/core/event.hpp>
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

using namespace fre;
using namespace std::chrono_literals;

// ─── Minimal synthetic evaluator and emission target ─────────────────────────

struct PassAllEvaluator {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "pass_all", .verdict = Verdict::Pass};
    }
};
static_assert(LightweightEvaluator<PassAllEvaluator>);

struct CollectingEmissionTarget {
    std::atomic<int>         emit_count{0};
    std::vector<Decision>    decisions;

    std::expected<void, EmissionError> emit(Decision d) {
        decisions.push_back(std::move(d));
        ++emit_count;
        return {};
    }
};
static_assert(EmissionTarget<CollectingEmissionTarget>);

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Pipeline contract: valid 3-stage config starts successfully", "[contract][pipeline][US1]") {
    GIVEN("a valid pipeline config with ingest, eval, and emit stages") {
        auto target = std::make_shared<CollectingEmissionTarget>();

        auto cfg_result = PipelineConfig::Builder()
            .pipeline_id("contract-test")
            .latency_budget(300ms)
            .ingest({})
            .lightweight_eval(
                EvalStageConfig{
                    .timeout      = 10ms,
                    .failure_mode = FailureMode::FailOpen,
                    .composition  = CompositionRule::AnyBlock,
                }
                .add_evaluator(PassAllEvaluator{})
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

        WHEN("the pipeline is started") {
            Pipeline pipeline{std::move(*cfg_result)};
            auto start_result = pipeline.start();

            THEN("start returns success") {
                REQUIRE(start_result.has_value());
            }

            WHEN("an event is submitted") {
                Event ev{
                    .tenant_id  = "acme",
                    .entity_id  = "user-1",
                    .event_type = "test_event",
                    .timestamp  = std::chrono::system_clock::now(),
                };
                auto submit_result = pipeline.submit(std::move(ev));

                THEN("submit returns without error") {
                    REQUIRE(submit_result.has_value());
                }

                THEN("at least one decision is emitted to the registered target") {
                    pipeline.drain(2s);
                    REQUIRE(target->emit_count.load() >= 1);
                }
            }

            pipeline.drain(2s);
        }
    }
}

TEST_CASE("Pipeline contract: missing emit stage causes startup failure", "[contract][pipeline][US1]") {
    GIVEN("a pipeline config missing the emit stage") {
        auto cfg_result = PipelineConfig::Builder()
            .pipeline_id("no-emit-test")
            .latency_budget(300ms)
            .ingest({})
            .build();

        THEN("build() returns RequiredStageMissing error") {
            REQUIRE_FALSE(cfg_result.has_value());
            REQUIRE(std::holds_alternative<ConfigError>(cfg_result.error()));
            const auto& err = std::get<ConfigError>(cfg_result.error());
            REQUIRE(err.code == ConfigErrorCode::RequiredStageMissing);
        }
    }
}
