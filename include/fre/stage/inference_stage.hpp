#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/decision.hpp>
#include <fre/core/event.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <expected>

namespace fre {

// ─── InferenceStage ──────────────────────────────────────────────────────────

/// The ML inference stage.
/// Invokes registered InferenceEvaluator implementations per event.
/// Enforces a per-stage timeout; on timeout, applies the configured FailureMode
/// and sets DegradedReason::EvaluatorTimeout.
///
/// WeightedScore composition: computes the average score across evaluators and
/// flags the event if the average exceeds score_threshold.
class InferenceStage {
public:
    explicit InferenceStage(InferenceStageConfig config);

    [[nodiscard]] std::expected<StageOutput, Error> process(const Event& event);

    static constexpr std::string_view stage_id() noexcept { return "inference"; }

private:
    InferenceStageConfig config_;

    [[nodiscard]] EvaluatorResult apply_failure_mode(
        const EvaluatorError& err, std::string_view evaluator_id) const noexcept;
};

}  // namespace fre
