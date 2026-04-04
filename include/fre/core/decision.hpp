#pragma once

#include <fre/core/decision_type.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace fre {

// ─── Decision ────────────────────────────────────────────────────────────────

/// The structured output record emitted for every event processed by the pipeline.
///
/// Invariants (from contracts/decision-record-contract.md):
///   - Every submitted event produces exactly one Decision.
///   - `final_verdict` equals max(stage verdicts).
///   - `elapsed_us` is the wall-clock duration from event receipt to emission enqueue.
///   - `stage_outputs` contains one entry per active stage that ran.
struct Decision {
    /// Pipeline that produced this decision.
    std::string pipeline_id;

    /// Pipeline version tag.
    std::string pipeline_version;

    /// The event that triggered this decision (by reference — caller owns storage).
    uint64_t event_id{0};

    /// Tenant the event belongs to.
    std::string tenant_id;

    /// Entity that was evaluated.
    std::string entity_id;

    /// Wall-clock time this decision was assembled.
    std::chrono::system_clock::time_point decided_at;

    /// Ordered stage outputs (one per active stage).
    std::vector<StageOutput> stage_outputs;

    /// Composed final verdict — max of all stage verdicts.
    Verdict final_verdict{Verdict::Pass};

    /// Aggregated degradation indicators across all stages.
    DegradedReason degraded_reason{DegradedReason::None};

    /// True if any stage was degraded.
    [[nodiscard]] bool is_degraded() const noexcept {
        return fre::is_degraded(degraded_reason);
    }

    /// End-to-end latency from event receipt to decision assembly (microseconds).
    uint64_t elapsed_us{0};

    /// Active decisions after combinability filtering, sorted by priority ascending
    /// (highest-precedence first). Populated by compute_active_decisions().
    /// Empty when no policy stage is configured or no decision_type_ids are assigned.
    std::vector<ActiveDecision> active_decisions;

    // ─── Helpers ─────────────────────────────────────────────────────────────

    /// Compute final_verdict from stage_outputs (call after all stages complete).
    void compute_final_verdict() noexcept {
        final_verdict = Verdict::Pass;
        for (const auto& s : stage_outputs) {
            final_verdict = max_verdict(final_verdict, s.verdict);
        }
    }

    /// Merge degraded_reason from stage_outputs.
    void merge_degraded_reasons() noexcept {
        degraded_reason = DegradedReason::None;
        for (const auto& s : stage_outputs) {
            degraded_reason |= s.degraded_reason;
        }
    }

    /// Returns pointer to a stage output by stage_id, or nullptr if absent.
    [[nodiscard]] const StageOutput* stage_output(std::string_view stage_id) const noexcept {
        for (const auto& s : stage_outputs) {
            if (s.stage_id == stage_id) return &s;
        }
        return nullptr;
    }

    /// Populate active_decisions from the policy stage's evaluator results.
    /// Reads EvaluatorResults that carry a decision_type_id (set by the policy
    /// stage's multi-decision path), resolves priorities from the registry, and
    /// sorts by priority ascending. No-op if registry is nullptr.
    void compute_active_decisions(const DecisionTypeRegistry* registry) noexcept;
};

}  // namespace fre
