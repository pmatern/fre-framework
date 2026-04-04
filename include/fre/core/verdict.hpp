#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fre {

// ─── Verdict ─────────────────────────────────────────────────────────────────

enum class Verdict : uint8_t {
    Pass  = 0,
    Flag  = 1,
    Block = 2,
};

/// Returns the stricter of two verdicts (higher ordinal wins).
[[nodiscard]] constexpr Verdict max_verdict(Verdict a, Verdict b) noexcept {
    return static_cast<Verdict>(
        std::max(static_cast<uint8_t>(a), static_cast<uint8_t>(b)));
}

// ─── DegradedReason ──────────────────────────────────────────────────────────

enum class DegradedReason : uint16_t {
    None                 = 0,
    EvaluatorTimeout     = 1 << 0,
    EvaluatorError       = 1 << 1,
    StateStoreUnavailable = 1 << 2,
    EmissionRetryExhausted = 1 << 3,
    RateLimitExhausted   = 1 << 4,
    IngestValidationFailed = 1 << 5,
};

[[nodiscard]] constexpr DegradedReason operator|(
    DegradedReason a, DegradedReason b) noexcept {
    return static_cast<DegradedReason>(
        static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}

[[nodiscard]] constexpr DegradedReason operator&(
    DegradedReason a, DegradedReason b) noexcept {
    return static_cast<DegradedReason>(
        static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}

constexpr DegradedReason& operator|=(DegradedReason& a, DegradedReason b) noexcept {
    a = a | b;
    return a;
}

[[nodiscard]] constexpr bool is_degraded(DegradedReason r) noexcept {
    return r != DegradedReason::None;
}

// ─── EvaluatorResult ─────────────────────────────────────────────────────────

struct EvaluatorResult {
    /// Unique identifier of the evaluator that produced this result.
    std::string evaluator_id;

    /// Verdict from this evaluator.
    Verdict verdict{Verdict::Pass};

    /// Optional human-readable reason code (e.g., "threshold_exceeded").
    std::optional<std::string> reason_code;

    /// For ML inference evaluators: raw anomaly score [0.0, 1.0].
    std::optional<float> score;

    /// True if this evaluator was skipped due to failure mode (FailOpen/FailClosed).
    bool skipped{false};

    /// Optional opaque metadata string (JSON or plain text).
    std::optional<std::string> metadata;

    /// For policy stage results: the registered decision type ID, if this
    /// evaluator result was produced by a multi-decision rule.
    /// nullopt for all eval/inference results and legacy policy rules.
    std::optional<std::string> decision_type_id;
};

// ─── CompositionRule ─────────────────────────────────────────────────────────

enum class CompositionRule : uint8_t {
    /// Block if any evaluator blocks.
    AnyBlock,
    /// Flag if any evaluator flags (block if any blocks).
    AnyFlag,
    /// Block only if all evaluators block.
    Unanimous,
    /// Block if strictly more than half of evaluators block.
    Majority,
    /// For inference stages: compare weighted average score against threshold.
    WeightedScore,
};

// ─── StageOutput ─────────────────────────────────────────────────────────────

struct StageOutput {
    /// Identifies which stage produced this output (e.g., "ingest", "eval", "inference").
    std::string stage_id;

    /// Composed verdict for this stage.
    Verdict verdict{Verdict::Pass};

    /// Per-evaluator results (empty if stage was skipped/degraded).
    std::vector<EvaluatorResult> evaluator_results;

    /// Degradation indicators for this stage.
    DegradedReason degraded_reason{DegradedReason::None};

    /// Processing duration for this stage in microseconds.
    uint64_t elapsed_us{0};
};

}  // namespace fre
