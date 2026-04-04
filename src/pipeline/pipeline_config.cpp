#include <fre/pipeline/pipeline_config.hpp>
#include <fre/sharding/tenant_router.hpp>

#include <format>
#include <numeric>
#include <set>

namespace fre {

namespace {

[[nodiscard]] std::chrono::milliseconds sum_stage_timeouts(const PipelineConfig& c) {
    using ms = std::chrono::milliseconds;
    ms total = c.ingest_config.timeout + c.emit_config.timeout;
    if (c.eval_config)      total += c.eval_config->timeout;
    if (c.inference_config) total += c.inference_config->timeout;
    if (c.policy_config)    total += c.policy_config->timeout;
    return total;
}

/// Walk a PolicyRule AST and collect all stage IDs referenced by StageVerdictIs nodes.
void collect_stage_refs(const policy::PolicyRule& rule,
                        std::set<std::string>& out)
{
    std::visit([&out](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, policy::StageVerdictIs>) {
            out.insert(node.stage_id);
        } else if constexpr (std::is_same_v<T, policy::And>) {
            if (node.left)  collect_stage_refs(*node.left,  out);
            if (node.right) collect_stage_refs(*node.right, out);
        } else if constexpr (std::is_same_v<T, policy::Or>) {
            if (node.left)  collect_stage_refs(*node.left,  out);
            if (node.right) collect_stage_refs(*node.right, out);
        } else if constexpr (std::is_same_v<T, policy::Not>) {
            if (node.expr)  collect_stage_refs(*node.expr,  out);
        }
        // StageVerdictIs, EvaluatorScoreAbove, TagEquals: leaf nodes
    }, rule.node);
}

}  // namespace

std::optional<ConfigError> PipelineConfig::Builder::validate() const {
    // Emit stage is required
    if (!has_emit_ || config_.emit_config.targets.empty()) {
        return ConfigError{
            ConfigErrorCode::RequiredStageMissing,
            "emit stage must be configured with at least one target"
        };
    }

    // Stage timeout sum must not exceed latency budget
    const auto total = sum_stage_timeouts(config_);
    if (total > config_.latency_budget) {
        return ConfigError{
            ConfigErrorCode::LatencyBudgetExceeded,
            std::format("stage timeouts sum to {}ms, exceeding budget of {}ms",
                        total.count(), config_.latency_budget.count())
        };
    }

    // Sharding: K must be < N
    if (config_.sharding.cells_per_tenant >= config_.sharding.num_cells) {
        return ConfigError{
            ConfigErrorCode::InvalidShardingConfig,
            std::format("cells_per_tenant ({}) must be < num_cells ({})",
                        config_.sharding.cells_per_tenant, config_.sharding.num_cells)
        };
    }

    // Policy stage: all StageVerdictIs references must point to a configured stage.
    if (config_.policy_config) {
        // Build the set of stage IDs that will actually run.
        std::set<std::string> active_stages{"ingest", "emit"};
        if (config_.eval_config)      active_stages.insert("eval");
        if (config_.inference_config) active_stages.insert("inference");

        for (const auto& psr : config_.policy_config->rules) {
            std::set<std::string> refs;
            collect_stage_refs(psr.rule, refs);
            for (const auto& ref : refs) {
                if (!active_stages.contains(ref)) {
                    return ConfigError{
                        ConfigErrorCode::UndefinedStageDependency,
                        std::format("policy rule '{}' references undefined stage '{}'",
                                    psr.rule_id.empty() ? "(unnamed)" : psr.rule_id, ref)
                    };
                }
            }

            // Any rule with a decision_type_id must reference a registered type.
            if (psr.decision_type_id.has_value()) {
                if (!config_.decision_type_registry.has_value()) {
                    return ConfigError{
                        ConfigErrorCode::InvalidPolicyRule,
                        std::format("policy rule '{}' has a decision_type_id but no "
                                    "DecisionTypeRegistry is configured on the pipeline",
                                    psr.rule_id.empty() ? "(unnamed)" : psr.rule_id)
                    };
                }
                if (!config_.decision_type_registry->find(*psr.decision_type_id)) {
                    return ConfigError{
                        ConfigErrorCode::InvalidPolicyRule,
                        std::format("policy rule '{}' references unregistered decision type '{}'",
                                    psr.rule_id.empty() ? "(unnamed)" : psr.rule_id,
                                    *psr.decision_type_id)
                    };
                }
            }
        }
    }

    return std::nullopt;
}

std::expected<PipelineConfig, Error> PipelineConfig::Builder::build() {
    if (auto err = validate()) {
        return std::unexpected(Error{std::move(*err)});
    }
    return std::move(config_);
}

PipelineConfig::Builder& PipelineConfig::Builder::sharding(ShardingConfig cfg) {
    config_.sharding = std::move(cfg);
    return *this;
}

PipelineConfig::Builder& PipelineConfig::Builder::rate_limit(RateLimitConfig cfg) {
    config_.rate_limit = std::move(cfg);
    return *this;
}

}  // namespace fre
