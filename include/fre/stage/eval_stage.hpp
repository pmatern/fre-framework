#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/decision.hpp>
#include <fre/core/event.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <expected>

namespace fre {

// ─── EvalStage ───────────────────────────────────────────────────────────────

/// The lightweight evaluation stage.
/// Runs all registered evaluators in sequence, composes their verdicts
/// according to the configured CompositionRule, and returns a StageOutput.
class EvalStage {
public:
    explicit EvalStage(EvalStageConfig config);

    /// Run all evaluators against the event.
    /// Per-evaluator errors trigger the configured FailureMode; the stage
    /// never propagates exceptions or aborts concurrent evaluations.
    [[nodiscard]] std::expected<StageOutput, Error> process(const Event& event);

    static constexpr std::string_view stage_id() noexcept { return "eval"; }

private:
    EvalStageConfig config_;

    [[nodiscard]] Verdict compose(const std::vector<EvaluatorResult>& results) const noexcept;

    [[nodiscard]] EvaluatorResult apply_failure_mode(
        const EvaluatorError& err, std::string_view evaluator_id) const noexcept;
};

}  // namespace fre
