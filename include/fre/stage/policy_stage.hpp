#pragma once

#include <fre/core/error.hpp>
#include <fre/core/verdict.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <expected>

namespace fre {

// ─── PolicyStage ─────────────────────────────────────────────────────────────

class PolicyStage {
public:
    explicit PolicyStage(PolicyStageConfig config);

    [[nodiscard]] static constexpr std::string_view stage_id() noexcept { return "policy"; }

    /// Evaluate all rules against the given PolicyContext.
    /// The first matching rule (by priority) determines the output verdict.
    /// Returns StageOutput; never throws.
    [[nodiscard]] std::expected<StageOutput, Error>
    process(const PolicyContext& ctx);

private:
    PolicyStageConfig config_;
};

}  // namespace fre
