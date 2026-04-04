#pragma once

#include <fre/core/decision_type.hpp>
#include <fre/core/error.hpp>
#include <fre/core/verdict.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <expected>

namespace fre {

// ─── PolicyStage ─────────────────────────────────────────────────────────────

class PolicyStage {
public:
    /// Construct a PolicyStage with an optional decision type registry.
    /// When registry is non-null and any rule carries a decision_type_id,
    /// all matching rules are evaluated (not just the first), and combinability
    /// constraints from the registry are applied before emitting results.
    /// When registry is null, legacy first-match-wins semantics are preserved.
    explicit PolicyStage(PolicyStageConfig config,
                         const DecisionTypeRegistry* registry = nullptr);

    [[nodiscard]] static constexpr std::string_view stage_id() noexcept { return "policy"; }

    /// Evaluate rules against the given PolicyContext.
    /// Returns StageOutput; never throws.
    [[nodiscard]] std::expected<StageOutput, Error>
    process(const PolicyContext& ctx);

private:
    PolicyStageConfig           config_;
    const DecisionTypeRegistry* registry_{nullptr};
};

}  // namespace fre
