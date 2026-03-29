#pragma once

/// WindowedHistoricalEvaluator — pattern for long-horizon aggregate queries.
///
/// Demonstrates how to build an evaluator that uses query_range() on a
/// DuckDbWindowStore (or any store with the same method) to answer questions
/// like "what was this entity's total event count over the past 30 days?"
///
/// This evaluator satisfies the LightweightEvaluator concept.
///
/// IMPORTANT: query_range() queries DuckDB (warm tier + parquet archive) and
/// is NOT sub-millisecond.  Schedule this evaluator on a stage with a generous
/// timeout or dispatch it asynchronously.  Never use it on the critical path
/// if latency budget is < 10ms.
///
/// Usage:
///   auto hist_eval = WindowedHistoricalEvaluator{store, "txn_count_30d",
///                                                 window_ms, num_windows,
///                                                 threshold};
///   eval_config.add_evaluator(std::ref(hist_eval));

#include <fre/core/concepts.hpp>
#include <fre/core/error.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>

namespace fre {

/// Concept for stores that support long-horizon range queries.
template <typename S>
concept RangeQueryStore = requires(S s, std::string_view sv, uint64_t u) {
    { s.query_range(sv, sv, sv, u, u) } -> std::same_as<std::expected<double, StoreError>>;
};

// ─── WindowedHistoricalEvaluator ─────────────────────────────────────────────

/// Evaluates an event by summing aggregate values across a historical range of
/// epochs and comparing the total against a threshold.
///
/// Template parameter Store must satisfy RangeQueryStore (e.g. DuckDbWindowStore).
template <RangeQueryStore Store>
class WindowedHistoricalEvaluator {
public:
    /// @param store         Store with query_range() support.
    /// @param window_name   Name of the window (must match the one written by ThresholdEvaluator).
    /// @param window_ms     Window duration in milliseconds (epoch size).
    /// @param num_windows   How many windows back to query (e.g. 30*24*60 for 30-day / 1-min windows).
    /// @param threshold     Flag if sum >= threshold.
    WindowedHistoricalEvaluator(Store& store,
                                 std::string window_name,
                                 uint64_t    window_ms,
                                 uint32_t    num_windows,
                                 double      threshold)
        : store_{store}
        , window_name_{std::move(window_name)}
        , window_ms_{window_ms}
        , num_windows_{num_windows}
        , threshold_{threshold}
    {}

    // ─── LightweightEvaluator concept ────────────────────────────────────────

    [[nodiscard]] std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& event) {
        const uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
            .count());

        const uint64_t current_epoch = (window_ms_ > 0) ? (now_ms / window_ms_) : 0;
        const uint64_t start_epoch   = (current_epoch >= num_windows_)
                                           ? (current_epoch - num_windows_ + 1)
                                           : 0;

        auto result = store_.query_range(
            event.tenant_id, event.entity_id, window_name_,
            start_epoch, current_epoch);

        if (!result.has_value()) {
            return std::unexpected(EvaluatorError{
                .code         = EvaluatorErrorCode::InternalError,
                .evaluator_id = "WindowedHistoricalEvaluator",
                .detail       = result.error().detail,
            });
        }

        return EvaluatorResult{
            .evaluator_id = "WindowedHistoricalEvaluator",
            .verdict      = (*result >= threshold_) ? Verdict::Flag : Verdict::Pass,
        };
    }

private:
    Store&      store_;
    std::string window_name_;
    uint64_t    window_ms_;
    uint32_t    num_windows_;
    double      threshold_;
};

// Concept satisfaction checks
#ifdef FRE_ENABLE_DUCKDB
#include <fre/state/duckdb_window_store.hpp>
#include <fre/state/write_back_window_store.hpp>
static_assert(LightweightEvaluator<WindowedHistoricalEvaluator<DuckDbWindowStore>>);
static_assert(LightweightEvaluator<WindowedHistoricalEvaluator<WriteBackWindowStore>>);
#endif

}  // namespace fre
