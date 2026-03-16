#pragma once

#include <fre/core/decision.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <expected>

namespace fre {

// ─── IngestStage ─────────────────────────────────────────────────────────────

/// The first stage in the pipeline.
/// Validates event fields and checks for late arrivals against skew_tolerance.
/// Always produces a StageOutput; failures result in EmitDegraded, not pipeline abort.
class IngestStage {
public:
    explicit IngestStage(IngestStageConfig config);

    /// Run ingest validation synchronously.
    /// Returns a StageOutput (possibly degraded) — never propagates errors as exceptions.
    [[nodiscard]] std::expected<StageOutput, Error> process(const Event& event);

    static constexpr std::string_view stage_id() noexcept { return "ingest"; }

private:
    IngestStageConfig config_;
};

}  // namespace fre
