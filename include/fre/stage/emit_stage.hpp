#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/decision.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <expected>

namespace fre {

// ─── StdoutEmissionTarget ────────────────────────────────────────────────────

/// Built-in emission target that writes a summary line to stdout.
/// Primarily for development and testing.
struct StdoutEmissionTarget {
    std::expected<void, EmissionError> emit(Decision d);
};
static_assert(EmissionTarget<StdoutEmissionTarget>);

// ─── EmitStage ───────────────────────────────────────────────────────────────

/// The decision emission stage.
/// Calls each registered EmissionTarget::emit() in order.
/// Retries on failure up to retry_limit with exponential back-off.
/// Dropped decisions (after max retries) increment a counter and are recorded
/// in the audit log.
class EmitStage {
public:
    explicit EmitStage(EmitStageConfig config);

    /// Emit the decision to all registered targets.
    /// Returns a StageOutput summarising emission results.
    [[nodiscard]] std::expected<StageOutput, Error> process(Decision decision);

    static constexpr std::string_view stage_id() noexcept { return "emit"; }

private:
    EmitStageConfig config_;
    uint64_t        dropped_decisions_{0};
};

}  // namespace fre
