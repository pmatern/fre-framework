#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/error.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace fre {

// ─── AllowDenyMatchField ──────────────────────────────────────────────────────

enum class AllowDenyMatchField : uint8_t {
    EntityId,
    TenantId,
};

// ─── AllowDenyEvaluatorConfig ────────────────────────────────────────────────

struct AllowDenyEvaluatorConfig {
    /// Path to the allow-list file (one entry per line). Empty = no allow-list.
    std::filesystem::path allow_list_path;

    /// Path to the deny-list file (one entry per line). Empty = no deny-list.
    std::filesystem::path deny_list_path;

    /// Which event field to match against the lists.
    AllowDenyMatchField match_field{AllowDenyMatchField::EntityId};

    /// Verdict to return when the entity is on neither list.
    Verdict default_verdict{Verdict::Pass};

    std::string evaluator_id{"allow_deny"};
};

// ─── AllowDenyEvaluator ───────────────────────────────────────────────────────

/// O(1) allow/deny list evaluator satisfying LightweightEvaluator.
///
/// Priority:
///   1. Allow-list match  → Pass  (unconditional allow)
///   2. Deny-list match   → Block (unconditional deny)
///   3. Neither           → configured default verdict
///
/// Lists are loaded at construction time; file access is NOT thread-safe
/// on construction but evaluate() IS thread-safe after construction.
class AllowDenyEvaluator {
public:
    /// Constructs the evaluator and loads both lists from disk.
    /// Throws std::runtime_error if a non-empty path does not exist or is unreadable.
    explicit AllowDenyEvaluator(AllowDenyEvaluatorConfig config);

    [[nodiscard]] std::expected<EvaluatorResult, EvaluatorError>
    evaluate(const Event& event) const;

    [[nodiscard]] std::string_view evaluator_id() const noexcept {
        return config_.evaluator_id;
    }

private:
    AllowDenyEvaluatorConfig     config_;
    std::unordered_set<std::string> allow_set_;
    std::unordered_set<std::string> deny_set_;

    static std::unordered_set<std::string>
    load_list(const std::filesystem::path& path);

    [[nodiscard]] std::string_view match_key(const Event& event) const noexcept;
};

static_assert(LightweightEvaluator<AllowDenyEvaluator>,
    "AllowDenyEvaluator must satisfy fre::LightweightEvaluator");

}  // namespace fre
