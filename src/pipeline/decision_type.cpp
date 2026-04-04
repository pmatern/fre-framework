#include <fre/core/decision_type.hpp>
#include <fre/core/decision.hpp>

#include <algorithm>

namespace fre {

// ─── DecisionTypeRegistry ────────────────────────────────────────────────────

std::expected<void, ConfigError>
DecisionTypeRegistry::add_type(DecisionTypeDescriptor desc) {
    if (desc.id.empty()) {
        return std::unexpected(ConfigError{
            ConfigErrorCode::InvalidPolicyRule,
            "decision type id must not be empty"});
    }
    if (find(desc.id)) {
        return std::unexpected(ConfigError{
            ConfigErrorCode::InvalidPolicyRule,
            "decision type '" + desc.id + "' is already registered"});
    }
    // Insert in sorted position (ascending priority) to keep types_ ordered.
    const auto it = std::lower_bound(types_.begin(), types_.end(), desc,
        [](const DecisionTypeDescriptor& existing, const DecisionTypeDescriptor& incoming) {
            return existing.priority < incoming.priority;
        });
    types_.insert(it, std::move(desc));
    return {};
}

std::expected<void, ConfigError>
DecisionTypeRegistry::add_incompatible(std::string type_id_a, std::string type_id_b) {
    if (!find(type_id_a)) {
        return std::unexpected(ConfigError{
            ConfigErrorCode::InvalidPolicyRule,
            "unknown decision type '" + type_id_a + "' in incompatible pair"});
    }
    if (!find(type_id_b)) {
        return std::unexpected(ConfigError{
            ConfigErrorCode::InvalidPolicyRule,
            "unknown decision type '" + type_id_b + "' in incompatible pair"});
    }
    incompatible_.push_back(
        IncompatiblePair{std::move(type_id_a), std::move(type_id_b)});
    return {};
}

const DecisionTypeDescriptor*
DecisionTypeRegistry::find(std::string_view type_id) const noexcept {
    for (const auto& desc : types_) {
        if (desc.id == type_id) return &desc;
    }
    return nullptr;
}

bool DecisionTypeRegistry::are_incompatible(std::string_view lhs,
                                             std::string_view rhs) const noexcept {
    for (const auto& pair : incompatible_) {
        if ((pair.type_id_a == lhs && pair.type_id_b == rhs) ||
            (pair.type_id_a == rhs && pair.type_id_b == lhs)) {
            return true;
        }
    }
    return false;
}

// ─── Decision::compute_active_decisions ──────────────────────────────────────

void Decision::compute_active_decisions(const DecisionTypeRegistry* registry) noexcept {
    if (!registry) return;

    const StageOutput* policy_out = stage_output("policy");
    if (!policy_out) return;

    active_decisions.clear();
    for (const auto& er : policy_out->evaluator_results) {
        if (!er.decision_type_id.has_value()) continue;
        const auto* desc = registry->find(*er.decision_type_id);
        if (!desc) continue;

        active_decisions.push_back(ActiveDecision{
            .decision_type_id = *er.decision_type_id,
            .rule_id          = er.evaluator_id,
            .priority         = desc->priority,
        });
    }

    // Sort by priority ascending (lowest number = highest precedence first).
    std::sort(active_decisions.begin(), active_decisions.end(),
        [](const ActiveDecision& lhs, const ActiveDecision& rhs) {
            return lhs.priority < rhs.priority;
        });
}

}  // namespace fre
