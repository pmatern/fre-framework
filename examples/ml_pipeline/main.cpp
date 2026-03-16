/// ML inference pipeline example: ingest → eval → inference → policy → emit
///
/// Demonstrates:
///  - Attaching an InferenceEvaluator (stub; replace with OnnxInferenceEvaluator)
///  - Configuring score_threshold for WeightedScore composition
///  - Declarative PolicyRule AST (And node)
///  - PipelineTestHarness for collecting decisions in tests/examples
///
/// quickstart.md Steps 4–5 reference.

#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/policy/rule_engine.hpp>
#include <fre/testing/pipeline_harness.hpp>
#include <fre/stage/emit_stage.hpp>

#include <chrono>
#include <iostream>

using namespace fre;
using namespace fre::policy;
using namespace std::chrono_literals;

// ─── Stub InferenceEvaluator ──────────────────────────────────────────────────
//
// Replace this with OnnxInferenceEvaluator (built with FRE_ENABLE_ONNX=ON)
// for real model inference.

struct AnomalyScoreStub {
    float fixed_score;

    std::expected<EvaluatorResult, EvaluatorError>
    evaluate(const Event& /*event*/) const noexcept {
        EvaluatorResult r;
        r.evaluator_id = "anomaly_stub";
        r.verdict      = Verdict::Pass;
        r.score        = fixed_score;
        return r;
    }
};
static_assert(InferenceEvaluator<AnomalyScoreStub>);

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    // ─── Configure ───────────────────────────────────────────────────────────

    EvalStageConfig eval_cfg;
    // Flag any event whose entity starts with "suspicious"
    eval_cfg.add_evaluator([](const Event& ev) -> std::expected<EvaluatorResult, EvaluatorError> {
        EvaluatorResult r;
        r.evaluator_id = "suspicious_prefix";
        r.verdict = (ev.entity_id.find("suspicious") != std::string_view::npos)
                    ? Verdict::Flag : Verdict::Pass;
        r.reason_code = (r.verdict == Verdict::Flag) ? "suspicious_entity" : "clean";
        return r;
    });

    InferenceStageConfig inf_cfg;
    inf_cfg.add_evaluator(AnomalyScoreStub{.fixed_score = 0.92f});
    inf_cfg.score_threshold = 0.75f;   // scores > 0.75 → Flag
    inf_cfg.timeout         = 100ms;

    PolicyStageConfig policy_cfg;
    // Block if eval stage flagged AND anomaly score > 0.8
    policy_cfg.add_rule(
        PolicyRule{And{
            StageVerdictIs{"eval", Verdict::Flag},
            EvaluatorScoreAbove{"anomaly_stub", 0.8f},
        }},
        /*priority=*/    1,
        /*action_verdict=*/ Verdict::Block,
        /*rule_id=*/     "block_suspicious_high_anomaly");

    auto cfg_result = PipelineConfig::Builder{}
        .pipeline_id("ml-example")
        .eval_config(std::move(eval_cfg))
        .inference_config(std::move(inf_cfg))
        .policy_config(std::move(policy_cfg))
        .emit_config(EmitStageConfig{}.add_stdout_target())
        .build();

    if (!cfg_result.has_value()) {
        std::cerr << "Config error: " << error_message(cfg_result.error()) << '\n';
        return 1;
    }

    // ─── Run via PipelineTestHarness for easy decision collection ─────────────

    fre::testing::PipelineTestHarness harness{std::move(*cfg_result)};

    if (auto r = harness.start(); !r.has_value()) {
        std::cerr << "Start error: " << error_message(r.error()) << '\n';
        return 1;
    }

    // Submit events
    std::array events = {
        std::pair<std::string_view, std::string_view>{"acme",  "clean-user"},
        std::pair<std::string_view, std::string_view>{"acme",  "suspicious-bot-42"},
        std::pair<std::string_view, std::string_view>{"acme",  "suspicious-crawler"},
    };

    for (const auto& [tenant, entity] : events) {
        Event ev{};
        ev.tenant_id  = tenant;
        ev.entity_id  = entity;
        ev.event_type = "api_call";
        ev.timestamp  = std::chrono::system_clock::now();

        if (auto r = harness.submit_events(std::span<const Event>{&ev, 1}); !r.has_value()) {
            std::cerr << "Submit error: " << error_message(r.error()) << '\n';
        }
    }

    // Wait for all decisions
    auto decisions_result = harness.wait_for_decisions(events.size(), 5000ms);
    if (!decisions_result.has_value()) {
        std::cerr << "Wait error: " << error_message(decisions_result.error()) << '\n';
        return 1;
    }

    std::cout << "\n─── Collected Decisions ───\n";
    for (const auto& d : *decisions_result) {
        const char* verdict_str =
            (d.final_verdict == Verdict::Block) ? "BLOCK" :
            (d.final_verdict == Verdict::Flag)  ? "FLAG"  : "PASS";

        std::cout << "  entity=" << d.entity_id
                  << " verdict=" << verdict_str
                  << " elapsed=" << d.elapsed_us << "µs"
                  << " degraded=" << (d.is_degraded() ? "yes" : "no")
                  << "\n";
    }

    harness.drain(3s);
    return 0;
}
