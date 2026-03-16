#include <fre/stage/eval_stage.hpp>
#include <fre/core/logging.hpp>

#include <string>

namespace fre {

EvalStage::EvalStage(EvalStageConfig config) : config_{std::move(config)} {}

std::expected<StageOutput, Error> EvalStage::process(const Event& event) {
    StageOutput out;
    out.stage_id = std::string{stage_id()};

    const auto start = std::chrono::steady_clock::now();

    std::vector<EvaluatorResult> results;
    results.reserve(config_.evaluators.size());

    for (std::size_t i = 0; i < config_.evaluators.size(); ++i) {
        const auto& fn = config_.evaluators[i];
        auto result = fn(event);

        if (result.has_value()) {
            results.push_back(std::move(*result));
        } else {
            // Apply failure mode for this evaluator
            const std::string eval_id = "evaluator_" + std::to_string(i);
            results.push_back(apply_failure_mode(result.error(), eval_id));

            // Set degraded reason on stage output
            out.degraded_reason |= DegradedReason::EvaluatorError;

            FRE_LOG_WARNING("Evaluator {} failed: {}", eval_id, result.error().message());
        }
    }

    out.evaluator_results = std::move(results);
    out.verdict           = compose(out.evaluator_results);

    const auto elapsed = std::chrono::steady_clock::now() - start;
    out.elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());

    return out;
}

Verdict EvalStage::compose(const std::vector<EvaluatorResult>& results) const noexcept {
    if (results.empty()) return Verdict::Pass;

    switch (config_.composition) {
        case CompositionRule::AnyBlock: {
            for (const auto& r : results) {
                if (r.verdict == Verdict::Block) return Verdict::Block;
            }
            for (const auto& r : results) {
                if (r.verdict == Verdict::Flag) return Verdict::Flag;
            }
            return Verdict::Pass;
        }
        case CompositionRule::AnyFlag: {
            Verdict best = Verdict::Pass;
            for (const auto& r : results) {
                best = max_verdict(best, r.verdict);
            }
            return best;
        }
        case CompositionRule::Unanimous: {
            bool all_block = true;
            for (const auto& r : results) {
                if (r.verdict != Verdict::Block) { all_block = false; break; }
            }
            return all_block ? Verdict::Block : Verdict::Pass;
        }
        case CompositionRule::Majority: {
            std::size_t block_count = 0;
            for (const auto& r : results) {
                if (r.verdict == Verdict::Block) ++block_count;
            }
            return (block_count * 2 > results.size()) ? Verdict::Block : Verdict::Pass;
        }
        default:
            return Verdict::Pass;
    }
}

EvaluatorResult EvalStage::apply_failure_mode(
    const EvaluatorError& /*err*/, std::string_view evaluator_id) const noexcept {
    EvaluatorResult r;
    r.evaluator_id = std::string{evaluator_id};
    r.skipped      = true;

    switch (config_.failure_mode) {
        case FailureMode::FailOpen:
            r.verdict     = Verdict::Pass;
            r.reason_code = "fail_open";
            break;
        case FailureMode::FailClosed:
            r.verdict     = Verdict::Block;
            r.reason_code = "fail_closed";
            break;
        case FailureMode::EmitDegraded:
            r.verdict     = Verdict::Pass;
            r.reason_code = "degraded";
            break;
    }
    return r;
}

}  // namespace fre
