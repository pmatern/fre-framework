#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>
#include <fre/state/window_store.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <string>

namespace fre {

// ─── ThresholdEvaluatorConfig ────────────────────────────────────────────────

struct ThresholdEvaluatorConfig {
    std::chrono::milliseconds window_duration{std::chrono::seconds{60}};
    AggregationFn             aggregation{AggregationFn::Count};
    GroupBy                   group_by{GroupBy::EntityId};
    double                    threshold{100.0};

    /// Tag key to group by when group_by == GroupBy::Tag.
    std::string group_by_tag;

    /// Unique name for this evaluator's window (used as part of WindowKey).
    std::string window_name{"default"};
};

// ─── ThresholdEvaluator ──────────────────────────────────────────────────────

/// Lightweight evaluator that flags entities when a windowed aggregation
/// crosses a configured threshold.
///
/// Satisfies LightweightEvaluator<ThresholdEvaluator>.
class ThresholdEvaluator {
public:
    ThresholdEvaluator(ThresholdEvaluatorConfig config,
                       std::shared_ptr<InProcessWindowStore> store);

    [[nodiscard]] std::expected<EvaluatorResult, EvaluatorError>
    evaluate(const Event& event);

    static constexpr std::string_view evaluator_id() noexcept {
        return "threshold_evaluator";
    }

private:
    ThresholdEvaluatorConfig                  config_;
    std::shared_ptr<InProcessWindowStore>     store_;

    [[nodiscard]] std::string entity_key(const Event& event) const;
    [[nodiscard]] uint64_t    current_epoch(const Event& event) const noexcept;
    [[nodiscard]] double      aggregate_value(double current, const Event& event) const noexcept;
};

static_assert(LightweightEvaluator<ThresholdEvaluator>);

}  // namespace fre
