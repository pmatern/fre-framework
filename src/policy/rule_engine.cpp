#include <fre/policy/rule_engine.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>

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

// ─── Internal helpers ─────────────────────────────────────────────────────────

namespace {

// Find a tag value by key; returns empty string_view if absent.
std::string_view find_tag(const fre::Event& event, std::string_view key) noexcept {
    for (const auto& tag : event.tags) {
        if (tag.key == key) return tag.value;
    }
    return {};
}

// Parse a string_view as double via strtod (non-throwing, no heap allocation).
// Returns {value, true} on success; {0.0, false} on failure or absent input.
std::pair<double, bool> parse_double(std::string_view sv) noexcept {
    if (sv.empty()) return {0.0, false};
    // strtod requires a null-terminated string; copy into a fixed stack buffer.
    // Numeric tag values are short; 64 bytes covers all realistic cases.
    char buf[64];
    if (sv.size() >= sizeof(buf)) return {0.0, false};
    std::memcpy(buf, sv.data(), sv.size());
    buf[sv.size()] = '\0';
    char* end{};
    const double val = std::strtod(buf, &end);
    // Ensure the entire string was consumed (no trailing garbage).
    if (end != buf + sv.size()) return {0.0, false};
    return {val, true};
}

}  // namespace

// ─── RuleEngine::evaluate ─────────────────────────────────────────────────────

bool RuleEngine::evaluate(const fre::PolicyContext& ctx, const PolicyRule& rule) {
    return std::visit([&ctx](const auto& node) -> bool {
        using T = std::decay_t<decltype(node)>;

        // ── Existing nodes ────────────────────────────────────────────────────

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

        // ── US1: Tag string matching ──────────────────────────────────────────

        } else if constexpr (std::is_same_v<T, TagContains>) {
            const auto val = find_tag(ctx.event, node.key);
            if (val.data() == nullptr && node.key.empty()) return false;
            // find_tag returns empty string_view for absent tag (data() points into
            // the tags span); distinguish absent from present-but-empty by checking
            // whether we actually found the key.
            for (const auto& tag : ctx.event.tags) {
                if (tag.key == node.key) {
                    return tag.value.find(node.substring) != std::string_view::npos;
                }
            }
            return false;

        } else if constexpr (std::is_same_v<T, TagStartsWith>) {
            for (const auto& tag : ctx.event.tags) {
                if (tag.key == node.key) {
                    return tag.value.starts_with(node.prefix);
                }
            }
            return false;

        } else if constexpr (std::is_same_v<T, TagIn>) {
            for (const auto& tag : ctx.event.tags) {
                if (tag.key == node.key) {
                    for (const auto& v : node.values) {
                        if (tag.value == v) return true;
                    }
                    return false;
                }
            }
            return false;

        } else if constexpr (std::is_same_v<T, TagExists>) {
            for (const auto& tag : ctx.event.tags) {
                if (tag.key == node.key) return true;
            }
            return false;

        // ── US2: Numeric tag comparisons ──────────────────────────────────────

        } else if constexpr (std::is_same_v<T, TagValueLessThan>) {
            for (const auto& tag : ctx.event.tags) {
                if (tag.key == node.key) {
                    auto [val, ok] = parse_double(tag.value);
                    return ok && val < node.threshold;
                }
            }
            return false;

        } else if constexpr (std::is_same_v<T, TagValueGreaterThan>) {
            for (const auto& tag : ctx.event.tags) {
                if (tag.key == node.key) {
                    auto [val, ok] = parse_double(tag.value);
                    return ok && val > node.threshold;
                }
            }
            return false;

        } else if constexpr (std::is_same_v<T, TagValueBetween>) {
            if (node.lo >= node.hi) return false;
            for (const auto& tag : ctx.event.tags) {
                if (tag.key == node.key) {
                    auto [val, ok] = parse_double(tag.value);
                    return ok && val >= node.lo && val < node.hi;
                }
            }
            return false;

        // ── US3: Event field matching ─────────────────────────────────────────

        } else if constexpr (std::is_same_v<T, EventTypeIs>) {
            return ctx.event.event_type == node.event_type;

        } else if constexpr (std::is_same_v<T, EventTypeIn>) {
            for (const auto& t : node.event_types) {
                if (ctx.event.event_type == t) return true;
            }
            return false;

        } else if constexpr (std::is_same_v<T, TenantIs>) {
            return ctx.event.tenant_id == node.tenant_id;

        } else if constexpr (std::is_same_v<T, EventOlderThan>) {
            const auto age = std::chrono::system_clock::now() - ctx.event.timestamp;
            if (age <= std::chrono::milliseconds::zero()) return false;
            return age > node.duration;

        } else if constexpr (std::is_same_v<T, EventNewerThan>) {
            const auto age = std::chrono::system_clock::now() - ctx.event.timestamp;
            if (age < std::chrono::milliseconds::zero()) return false;
            return age < node.duration;

        // ── US4: Evaluator score range and pipeline health ────────────────────

        } else if constexpr (std::is_same_v<T, EvaluatorScoreBetween>) {
            if (node.lo >= node.hi) return false;
            for (const auto& so : ctx.stage_outputs) {
                for (const auto& er : so.evaluator_results) {
                    if (er.evaluator_id == node.evaluator_id && er.score.has_value()) {
                        return *er.score >= node.lo && *er.score < node.hi;
                    }
                }
            }
            return false;

        } else if constexpr (std::is_same_v<T, StageIsDegraded>) {
            for (const auto& so : ctx.stage_outputs) {
                if (so.stage_id == node.stage_id) {
                    return fre::is_degraded(so.degraded_reason);
                }
            }
            return false;

        } else if constexpr (std::is_same_v<T, EvaluatorWasSkipped>) {
            for (const auto& so : ctx.stage_outputs) {
                for (const auto& er : so.evaluator_results) {
                    if (er.evaluator_id == node.evaluator_id) {
                        return er.skipped;
                    }
                }
            }
            return false;

        } else if constexpr (std::is_same_v<T, EvaluatorReasonIs>) {
            for (const auto& so : ctx.stage_outputs) {
                for (const auto& er : so.evaluator_results) {
                    if (er.evaluator_id == node.evaluator_id) {
                        return er.reason_code.has_value() &&
                               *er.reason_code == node.reason_code;
                    }
                }
            }
            return false;
        }

        return false;
    }, rule.node);
}

}  // namespace fre::policy
