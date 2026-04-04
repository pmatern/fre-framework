#pragma once

#include <fre/core/error.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fre {

// ─── DecisionTypeDescriptor ──────────────────────────────────────────────────

/// Describes a named decision type with an associated priority.
/// Lower priority value = higher precedence (0 is the highest).
struct DecisionTypeDescriptor {
    std::string id;
    std::string display_name;
    uint32_t    priority{0};
};

// ─── IncompatiblePair ────────────────────────────────────────────────────────

/// Declares that two decision types cannot coexist on the same event.
/// When both are triggered, the one with the lower priority value survives.
/// On equal priority, the earlier match (by rule evaluation order) survives.
struct IncompatiblePair {
    std::string type_id_a;
    std::string type_id_b;
};

// ─── DecisionTypeRegistry ────────────────────────────────────────────────────

/// Registry owned by PipelineConfig. Registers named decision types and their
/// combinability constraints. All methods are non-throwing.
class DecisionTypeRegistry {
public:
    /// Register a new decision type.
    /// Returns error if id is empty or already registered.
    [[nodiscard]] std::expected<void, ConfigError>
    add_type(DecisionTypeDescriptor desc);

    /// Declare that the two named types cannot coexist on the same event.
    /// Returns error if either type ID is not registered.
    [[nodiscard]] std::expected<void, ConfigError>
    add_incompatible(std::string type_id_a, std::string type_id_b);

    /// Returns a pointer to the descriptor, or nullptr if not found.
    [[nodiscard]] const DecisionTypeDescriptor*
    find(std::string_view type_id) const noexcept;

    /// Returns true if the two types are declared incompatible (order-independent).
    [[nodiscard]] bool
    are_incompatible(std::string_view lhs, std::string_view rhs) const noexcept;

    [[nodiscard]] std::span<const DecisionTypeDescriptor>
    types() const noexcept { return types_; }

    [[nodiscard]] std::span<const IncompatiblePair>
    incompatible_pairs() const noexcept { return incompatible_; }

private:
    std::vector<DecisionTypeDescriptor> types_;      ///< Sorted by priority ascending.
    std::vector<IncompatiblePair>       incompatible_;
};

// ─── ActiveDecision ──────────────────────────────────────────────────────────

/// One resolved decision on a Decision record, after combinability filtering.
/// Populated by Decision::compute_active_decisions().
struct ActiveDecision {
    std::string decision_type_id;  ///< Registered type id (e.g. "block", "notify").
    std::string rule_id;           ///< Policy rule that triggered this decision.
    uint32_t    priority{0};       ///< Denormalized from DecisionTypeDescriptor.
};

}  // namespace fre
