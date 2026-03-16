/// Minimal pipeline example: ingest → lightweight evaluation → emit
///
/// Demonstrates:
///  - PipelineConfig builder DSL
///  - ThresholdEvaluator configuration (count threshold on 60s window)
///  - StdoutEmissionTarget for development

#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/stage/emit_stage.hpp>

#include <chrono>
#include <iostream>
#include <memory>

using namespace fre;
using namespace std::chrono_literals;

// ─── Custom evaluator example ─────────────────────────────────────────────────

struct TrustedEntityAllowList {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& event) {
        if (event.entity_id == "trusted-user") {
            return EvaluatorResult{
                .evaluator_id = "allow_list",
                .verdict      = Verdict::Pass,
                .reason_code  = "trusted_entity",
            };
        }
        return EvaluatorResult{
            .evaluator_id = "allow_list",
            .verdict      = Verdict::Flag,
            .reason_code  = "unknown_entity",
        };
    }
};
static_assert(LightweightEvaluator<TrustedEntityAllowList>);

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    auto cfg_result = PipelineConfig::Builder()
        .pipeline_id("minimal-example")
        .latency_budget(300ms)
        .ingest(IngestStageConfig{.skew_tolerance = 30s, .timeout = 5ms})
        .lightweight_eval(
            EvalStageConfig{
                .timeout      = 10ms,
                .failure_mode = FailureMode::FailOpen,
                .composition  = CompositionRule::AnyBlock,
            }
            .add_evaluator(TrustedEntityAllowList{})
        )
        .emit(
            EmitStageConfig{
                .timeout      = 10ms,
                .failure_mode = FailureMode::EmitDegraded,
            }
            .add_stdout_target()
        )
        .build();

    if (!cfg_result) {
        std::cerr << "Config error: " << error_message(cfg_result.error()) << '\n';
        return 1;
    }

    Pipeline pipeline{std::move(*cfg_result)};

    if (auto start_result = pipeline.start(); !start_result) {
        std::cerr << "Start error: " << error_message(start_result.error()) << '\n';
        return 1;
    }

    // Submit a few events
    const std::string events[][2] = {
        {"acme", "trusted-user"},
        {"acme", "unknown-user"},
        {"acme", "trusted-user"},
    };

    for (const auto& [tenant, entity] : events) {
        Event ev{
            .tenant_id  = tenant,
            .entity_id  = entity,
            .event_type = "api_call",
            .timestamp  = std::chrono::system_clock::now(),
        };

        if (auto r = pipeline.submit(std::move(ev)); !r) {
            std::cerr << "Submit error: " << error_message(r.error()) << '\n';
        }
    }

    pipeline.drain(5s);
    return 0;
}
