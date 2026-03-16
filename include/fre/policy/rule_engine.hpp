#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>

#include <memory>
#include <string>
#include <variant>

namespace fre::policy {

// ─── PolicyRule AST node types ────────────────────────────────────────────────
//
// Each node type is a simple aggregate. Composite nodes (And, Or, Not) hold
// child PolicyRule values via unique_ptr to allow recursive types in std::variant.

struct PolicyRule;  // forward declaration for recursive variants

/// Matches if the named stage's StageOutput::verdict equals the given verdict.
struct StageVerdictIs {
    std::string    stage_id;
    fre::Verdict   verdict;
};

/// Matches if any EvaluatorResult in any stage output whose evaluator_id equals
/// the given id has a score >= threshold.
struct EvaluatorScoreAbove {
    std::string evaluator_id;
    float       threshold{0.0f};
};

/// Matches if the event carries a tag whose key and value both match exactly.
struct TagEquals {
    std::string key;
    std::string value;
};

/// Matches if both child rules match.
struct And {
    std::unique_ptr<PolicyRule> left;
    std::unique_ptr<PolicyRule> right;

    // Convenience constructor that accepts concrete PolicyRule values.
    template <typename L, typename R>
    And(L lhs, R rhs)
        : left{std::make_unique<PolicyRule>(std::move(lhs))}
        , right{std::make_unique<PolicyRule>(std::move(rhs))} {}

    And(const And& other);
    And& operator=(const And& other);
    And(And&&)            = default;
    And& operator=(And&&) = default;
};

/// Matches if at least one child rule matches.
struct Or {
    std::unique_ptr<PolicyRule> left;
    std::unique_ptr<PolicyRule> right;

    template <typename L, typename R>
    Or(L lhs, R rhs)
        : left{std::make_unique<PolicyRule>(std::move(lhs))}
        , right{std::make_unique<PolicyRule>(std::move(rhs))} {}

    Or(const Or& other);
    Or& operator=(const Or& other);
    Or(Or&&)            = default;
    Or& operator=(Or&&) = default;
};

/// Matches if the child rule does NOT match.
struct Not {
    std::unique_ptr<PolicyRule> expr;

    template <typename E>
    explicit Not(E e) : expr{std::make_unique<PolicyRule>(std::move(e))} {}

    Not(const Not& other);
    Not& operator=(const Not& other);
    Not(Not&&)            = default;
    Not& operator=(Not&&) = default;
};

// ─── PolicyRule variant ───────────────────────────────────────────────────────

struct PolicyRule {
    using Variant = std::variant<
        StageVerdictIs,
        EvaluatorScoreAbove,
        TagEquals,
        And,
        Or,
        Not>;

    Variant node;

    // Implicit conversions from leaf / composite node types
    // NOLINTBEGIN(google-explicit-constructor)
    PolicyRule(StageVerdictIs v)    : node{std::move(v)} {}  // NOLINT
    PolicyRule(EvaluatorScoreAbove v) : node{std::move(v)} {}  // NOLINT
    PolicyRule(TagEquals v)         : node{std::move(v)} {}  // NOLINT
    PolicyRule(And v)               : node{std::move(v)} {}  // NOLINT
    PolicyRule(Or v)                : node{std::move(v)} {}  // NOLINT
    PolicyRule(Not v)               : node{std::move(v)} {}  // NOLINT
    // NOLINTEND(google-explicit-constructor)

    PolicyRule(const PolicyRule& other);
    PolicyRule& operator=(const PolicyRule& other);
    PolicyRule(PolicyRule&&)            = default;
    PolicyRule& operator=(PolicyRule&&) = default;
};

// ─── RuleEngine ──────────────────────────────────────────────────────────────

/// Stateless rule evaluator — pure function over (PolicyContext, PolicyRule) → bool.
class RuleEngine {
public:
    RuleEngine() = delete;

    /// Returns true if the rule matches the given context; false otherwise.
    [[nodiscard]] static bool evaluate(const fre::PolicyContext& ctx, const PolicyRule& rule);
};

}  // namespace fre::policy
