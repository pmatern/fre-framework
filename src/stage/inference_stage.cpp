#include <fre/stage/inference_stage.hpp>
#include <fre/core/logging.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

namespace fre {

InferenceStage::InferenceStage(InferenceStageConfig config) : config_{std::move(config)} {}

std::expected<StageOutput, Error> InferenceStage::process(const Event& event) {
    StageOutput out;
    out.stage_id = std::string{stage_id()};

    const auto start = std::chrono::steady_clock::now();

    std::vector<EvaluatorResult> results;
    results.reserve(config_.evaluators.size());

    for (std::size_t i = 0; i < config_.evaluators.size(); ++i) {
        const auto& fn          = config_.evaluators[i];
        const std::string eval_id = "inference_evaluator_" + std::to_string(i);

        // ─── Timeout enforcement ─────────────────────────────────────────────
        // Run the evaluator in a thread with a deadline check.
        // If the evaluator exceeds its timeout, apply FailureMode.

        std::optional<std::expected<EvaluatorResult, EvaluatorError>> result_holder;
        std::atomic<bool> completed{false};

        std::thread worker([&] {
            result_holder = fn(event);
            completed.store(true, std::memory_order_release);
        });

        const auto deadline = std::chrono::steady_clock::now() + config_.timeout;
        while (!completed.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                // Timeout — detach worker (it will run to completion independently)
                worker.detach();

                FRE_LOG_WARNING("Inference evaluator {} timed out after {}ms",
                                eval_id, config_.timeout.count());

                EvaluatorResult degraded = apply_failure_mode(
                    EvaluatorError{EvaluatorErrorCode::Timeout, eval_id, "timeout"},
                    eval_id);
                degraded.skipped = true;
                results.push_back(std::move(degraded));
                out.degraded_reason |= DegradedReason::EvaluatorTimeout;
                goto next_evaluator;  // NOLINT(cppcoreguidelines-avoid-goto)
            }
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }

        if (worker.joinable()) worker.join();

        if (result_holder.has_value()) {
            if (result_holder->has_value()) {
                results.push_back(std::move(**result_holder));
            } else {
                FRE_LOG_WARNING("Inference evaluator {} failed: {}",
                                eval_id, result_holder->error().message());
                results.push_back(apply_failure_mode(result_holder->error(), eval_id));
                out.degraded_reason |= DegradedReason::EvaluatorError;
            }
        }

        next_evaluator:;
    }

    out.evaluator_results = std::move(results);

    // ─── WeightedScore composition ───────────────────────────────────────────
    if (config_.composition == CompositionRule::WeightedScore && !out.evaluator_results.empty()) {
        float score_sum   = 0.0f;
        int   score_count = 0;
        for (const auto& r : out.evaluator_results) {
            if (r.score.has_value() && !r.skipped) {
                score_sum += *r.score;
                ++score_count;
            }
        }
        const float avg_score = (score_count > 0) ? score_sum / static_cast<float>(score_count) : 0.0f;
        out.verdict = (avg_score > config_.score_threshold) ? Verdict::Flag : Verdict::Pass;
    } else {
        // Fallback: use max verdict from results
        out.verdict = Verdict::Pass;
        for (const auto& r : out.evaluator_results) {
            out.verdict = max_verdict(out.verdict, r.verdict);
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    out.elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());

    return out;
}

EvaluatorResult InferenceStage::apply_failure_mode(
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
