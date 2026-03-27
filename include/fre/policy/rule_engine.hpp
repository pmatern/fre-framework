#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace fre::policy {

// ─── PolicyRule AST node types ────────────────────────────────────────────────
//
// Each node type is a simple aggregate. Composite nodes (And, Or, Not) hold
// child PolicyRule values via unique_ptr to allow recursive types in std::variant.
// All new leaf nodes are plain value types — no unique_ptr children, default copy.

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

// ─── New leaf nodes: tag string matching ──────────────────────────────────────

/// Matches if the named tag's value contains the given substring (case-sensitive).
struct TagContains {
    std::string key;
    std::string substring;
};

/// Matches if the named tag's value begins with the given prefix (case-sensitive).
struct TagStartsWith {
    std::string key;
    std::string prefix;
};

/// Matches if the named tag's value is an exact member of the given set.
struct TagIn {
    std::string              key;
    std::vector<std::string> values;
};

/// Matches if the event carries the named tag, regardless of its value.
struct TagExists {
    std::string key;
};

// ─── New leaf nodes: numeric tag comparisons ──────────────────────────────────
// Tag values are parsed as double via std::from_chars (non-throwing).
// Absent tag or parse failure → false.

/// Matches if the named tag's value, parsed as double, is strictly less than threshold.
struct TagValueLessThan {
    std::string key;
    double      threshold{0.0};
};

/// Matches if the named tag's value, parsed as double, is strictly greater than threshold.
struct TagValueGreaterThan {
    std::string key;
    double      threshold{0.0};
};

/// Matches if the named tag's value, parsed as double, satisfies lo <= value < hi.
struct TagValueBetween {
    std::string key;
    double      lo{0.0};
    double      hi{0.0};
};

// ─── New leaf nodes: first-class Event field matching ─────────────────────────

/// Matches if Event::event_type equals the given string exactly.
struct EventTypeIs {
    std::string event_type;
};

/// Matches if Event::event_type is a member of the given set.
struct EventTypeIn {
    std::vector<std::string> event_types;
};

/// Matches if Event::tenant_id equals the given string exactly.
struct TenantIs {
    std::string tenant_id;
};

/// Matches if (system_clock::now() - event.timestamp) > duration.
/// Future-timestamped events (negative age) never match.
struct EventOlderThan {
    std::chrono::milliseconds duration{0};
};

/// Matches if (system_clock::now() - event.timestamp) < duration.
/// Future-timestamped events (negative age) never match.
struct EventNewerThan {
    std::chrono::milliseconds duration{0};
};

// ─── New leaf nodes: evaluator score range and pipeline health ─────────────────

/// Matches if the named evaluator's score satisfies lo <= score < hi.
struct EvaluatorScoreBetween {
    std::string evaluator_id;
    float       lo{0.0f};
    float       hi{0.0f};
};

/// Matches if the named stage's degraded_reason is non-zero (i.e. is_degraded()).
struct StageIsDegraded {
    std::string stage_id;
};

/// Matches if any result for the named evaluator has skipped == true.
struct EvaluatorWasSkipped {
    std::string evaluator_id;
};

/// Matches if any result for the named evaluator has reason_code == the given string.
struct EvaluatorReasonIs {
    std::string evaluator_id;
    std::string reason_code;
};

// ─── PolicyRule variant ───────────────────────────────────────────────────────

struct PolicyRule {
    using Variant = std::variant<
        StageVerdictIs,
        EvaluatorScoreAbove,
        TagEquals,
        And,
        Or,
        Not,
        // Tag string matching
        TagContains,
        TagStartsWith,
        TagIn,
        TagExists,
        // Numeric tag comparisons
        TagValueLessThan,
        TagValueGreaterThan,
        TagValueBetween,
        // Event field matching
        EventTypeIs,
        EventTypeIn,
        TenantIs,
        EventOlderThan,
        EventNewerThan,
        // Evaluator score range and pipeline health
        EvaluatorScoreBetween,
        StageIsDegraded,
        EvaluatorWasSkipped,
        EvaluatorReasonIs>;

    Variant node;

    // Implicit conversions from all node types
    // NOLINTBEGIN(google-explicit-constructor)
    PolicyRule(StageVerdictIs v)       : node{std::move(v)} {}  // NOLINT
    PolicyRule(EvaluatorScoreAbove v)  : node{std::move(v)} {}  // NOLINT
    PolicyRule(TagEquals v)            : node{std::move(v)} {}  // NOLINT
    PolicyRule(And v)                  : node{std::move(v)} {}  // NOLINT
    PolicyRule(Or v)                   : node{std::move(v)} {}  // NOLINT
    PolicyRule(Not v)                  : node{std::move(v)} {}  // NOLINT
    PolicyRule(TagContains v)          : node{std::move(v)} {}  // NOLINT
    PolicyRule(TagStartsWith v)        : node{std::move(v)} {}  // NOLINT
    PolicyRule(TagIn v)                : node{std::move(v)} {}  // NOLINT
    PolicyRule(TagExists v)            : node{std::move(v)} {}  // NOLINT
    PolicyRule(TagValueLessThan v)     : node{std::move(v)} {}  // NOLINT
    PolicyRule(TagValueGreaterThan v)  : node{std::move(v)} {}  // NOLINT
    PolicyRule(TagValueBetween v)      : node{std::move(v)} {}  // NOLINT
    PolicyRule(EventTypeIs v)          : node{std::move(v)} {}  // NOLINT
    PolicyRule(EventTypeIn v)          : node{std::move(v)} {}  // NOLINT
    PolicyRule(TenantIs v)             : node{std::move(v)} {}  // NOLINT
    PolicyRule(EventOlderThan v)       : node{std::move(v)} {}  // NOLINT
    PolicyRule(EventNewerThan v)       : node{std::move(v)} {}  // NOLINT
    PolicyRule(EvaluatorScoreBetween v): node{std::move(v)} {}  // NOLINT
    PolicyRule(StageIsDegraded v)      : node{std::move(v)} {}  // NOLINT
    PolicyRule(EvaluatorWasSkipped v)  : node{std::move(v)} {}  // NOLINT
    PolicyRule(EvaluatorReasonIs v)    : node{std::move(v)} {}  // NOLINT
    // NOLINTEND(google-explicit-constructor)

    PolicyRule(const PolicyRule& other);
    PolicyRule& operator=(const PolicyRule& other);
    PolicyRule(PolicyRule&&)            = default;
    PolicyRule& operator=(PolicyRule&&) = default;
};

// FR-019: verify all new leaf nodes (and PolicyRule itself) are copy-constructible.
static_assert(std::is_copy_constructible_v<PolicyRule>);
static_assert(std::is_copy_constructible_v<TagContains>);
static_assert(std::is_copy_constructible_v<TagStartsWith>);
static_assert(std::is_copy_constructible_v<TagIn>);
static_assert(std::is_copy_constructible_v<TagExists>);
static_assert(std::is_copy_constructible_v<TagValueLessThan>);
static_assert(std::is_copy_constructible_v<TagValueGreaterThan>);
static_assert(std::is_copy_constructible_v<TagValueBetween>);
static_assert(std::is_copy_constructible_v<EventTypeIs>);
static_assert(std::is_copy_constructible_v<EventTypeIn>);
static_assert(std::is_copy_constructible_v<TenantIs>);
static_assert(std::is_copy_constructible_v<EventOlderThan>);
static_assert(std::is_copy_constructible_v<EventNewerThan>);
static_assert(std::is_copy_constructible_v<EvaluatorScoreBetween>);
static_assert(std::is_copy_constructible_v<StageIsDegraded>);
static_assert(std::is_copy_constructible_v<EvaluatorWasSkipped>);
static_assert(std::is_copy_constructible_v<EvaluatorReasonIs>);

// ─── RuleEngine ──────────────────────────────────────────────────────────────

/// Stateless rule evaluator — pure function over (PolicyContext, PolicyRule) → bool.
class RuleEngine {
public:
    RuleEngine() = delete;

    /// Returns true if the rule matches the given context; false otherwise.
    [[nodiscard]] static bool evaluate(const fre::PolicyContext& ctx, const PolicyRule& rule);
};

}  // namespace fre::policy
