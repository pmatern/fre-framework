#include <fre/stage/policy_stage.hpp>
#include <fre/core/logging.hpp>

#include <chrono>

namespace fre {

PolicyStage::PolicyStage(PolicyStageConfig config) : config_{std::move(config)} {}

std::expected<StageOutput, Error> PolicyStage::process(const PolicyContext& ctx) {
    const auto start = std::chrono::steady_clock::now();

    StageOutput out;
    out.stage_id = std::string{stage_id()};
    out.verdict  = Verdict::Pass;

    for (const PolicyStageRule& psr : config_.rules) {
        bool matched = false;
        try {
            matched = policy::RuleEngine::evaluate(ctx, psr.rule);
        } catch (...) {
            // Rule evaluation must never throw — treat as FailClosed per spec.
            EvaluatorResult degraded;
            degraded.evaluator_id = psr.rule_id.empty() ? "unknown_rule" : psr.rule_id;
            degraded.verdict      = (config_.failure_mode == FailureMode::FailClosed)
                                        ? Verdict::Block : Verdict::Pass;
            degraded.skipped      = true;
            degraded.reason_code  = "rule_eval_exception";
            out.evaluator_results.push_back(std::move(degraded));
            out.degraded_reason |= DegradedReason::EvaluatorError;
            continue;
        }

        if (matched) {
            EvaluatorResult er;
            er.evaluator_id = psr.rule_id.empty() ? "policy_rule" : psr.rule_id;
            er.verdict      = psr.action_verdict;
            er.reason_code  = psr.rule_id;

            out.verdict = max_verdict(out.verdict, psr.action_verdict);
            out.evaluator_results.push_back(std::move(er));

            // First matching rule wins; stop evaluating further rules.
            break;
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    out.elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());

    return out;
}

}  // namespace fre
