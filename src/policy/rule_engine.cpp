#include <fre/policy/rule_engine.hpp>

#include <algorithm>

namespace fre::policy {

// ─── Copy operations for recursive variant nodes ──────────────────────────────

PolicyRule::PolicyRule(const PolicyRule& other) : node{other.node} {}
PolicyRule& PolicyRule::operator=(const PolicyRule& other) {
    if (this != &other) {
        node = other.node;
    }
    return *this;
}

And::And(const And& other)
    : left{other.left  ? std::make_unique<PolicyRule>(*other.left)  : nullptr}
    , right{other.right ? std::make_unique<PolicyRule>(*other.right) : nullptr}
{}

And& And::operator=(const And& other) {
    if (this != &other) {
        left  = other.left  ? std::make_unique<PolicyRule>(*other.left)  : nullptr;
        right = other.right ? std::make_unique<PolicyRule>(*other.right) : nullptr;
    }
    return *this;
}

Or::Or(const Or& other)
    : left{other.left  ? std::make_unique<PolicyRule>(*other.left)  : nullptr}
    , right{other.right ? std::make_unique<PolicyRule>(*other.right) : nullptr}
{}

Or& Or::operator=(const Or& other) {
    if (this != &other) {
        left  = other.left  ? std::make_unique<PolicyRule>(*other.left)  : nullptr;
        right = other.right ? std::make_unique<PolicyRule>(*other.right) : nullptr;
    }
    return *this;
}

Not::Not(const Not& other)
    : expr{other.expr ? std::make_unique<PolicyRule>(*other.expr) : nullptr}
{}

Not& Not::operator=(const Not& other) {
    if (this != &other) {
        expr = other.expr ? std::make_unique<PolicyRule>(*other.expr) : nullptr;
    }
    return *this;
}

// ─── RuleEngine::evaluate ─────────────────────────────────────────────────────

bool RuleEngine::evaluate(const fre::PolicyContext& ctx, const PolicyRule& rule) {
    return std::visit([&ctx](const auto& node) -> bool {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, StageVerdictIs>) {
            for (const auto& so : ctx.stage_outputs) {
                if (so.stage_id == node.stage_id) {
                    return so.verdict == node.verdict;
                }
            }
            return false;

        } else if constexpr (std::is_same_v<T, EvaluatorScoreAbove>) {
            for (const auto& so : ctx.stage_outputs) {
                for (const auto& er : so.evaluator_results) {
                    if (er.evaluator_id == node.evaluator_id && er.score.has_value()) {
                        if (*er.score >= node.threshold) {
                            return true;
                        }
                    }
                }
            }
            return false;

        } else if constexpr (std::is_same_v<T, TagEquals>) {
            for (const auto& tag : ctx.event.tags) {
                if (tag.key == node.key && tag.value == node.value) {
                    return true;
                }
            }
            return false;

        } else if constexpr (std::is_same_v<T, And>) {
            if (!node.left || !node.right) return false;
            return RuleEngine::evaluate(ctx, *node.left) &&
                   RuleEngine::evaluate(ctx, *node.right);

        } else if constexpr (std::is_same_v<T, Or>) {
            if (!node.left || !node.right) return false;
            return RuleEngine::evaluate(ctx, *node.left) ||
                   RuleEngine::evaluate(ctx, *node.right);

        } else if constexpr (std::is_same_v<T, Not>) {
            if (!node.expr) return true;
            return !RuleEngine::evaluate(ctx, *node.expr);
        }

        return false;
    }, rule.node);
}

}  // namespace fre::policy
