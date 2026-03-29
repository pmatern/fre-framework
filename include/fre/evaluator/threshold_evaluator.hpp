#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>
#include <fre/state/window_store.hpp>

#include <chrono>
#include <expected>
#include <format>
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
/// Template parameter Store must satisfy StateStore (e.g. InProcessWindowStore,
/// WriteBackWindowStore).  CTAD deduces Store from the shared_ptr argument:
///   ThresholdEvaluator{config, make_shared<InProcessWindowStore>()}
///
/// Satisfies LightweightEvaluator<ThresholdEvaluator<Store>>.
template <StateStore Store>
class ThresholdEvaluator {
public:
    ThresholdEvaluator(ThresholdEvaluatorConfig config, std::shared_ptr<Store> store)
        : config_{std::move(config)}, store_{std::move(store)} {}

    [[nodiscard]] std::expected<EvaluatorResult, EvaluatorError>
    evaluate(const Event& event) {
        const WindowKey key{
            .tenant_id   = std::string{event.tenant_id},
            .entity_id   = entity_key(event),
            .window_name = config_.window_name,
            .epoch       = current_epoch(event),
        };

        bool        swapped = false;
        WindowValue current;
        WindowValue updated;

        for (int attempts = 0; attempts < 10 && !swapped; ++attempts) {
            auto get_result = store_->get(key);
            if (!get_result.has_value()) {
                return std::unexpected(EvaluatorError{
                    .code         = EvaluatorErrorCode::InternalError,
                    .evaluator_id = std::string{evaluator_id()},
                    .detail       = get_result.error().detail,
                });
            }
            current = *get_result;
            updated = WindowValue{
                .aggregate = aggregate_value(current.aggregate, event),
                .version   = current.version + 1,
            };

            auto cas_result = store_->compare_and_swap(key, current, updated);
            if (!cas_result.has_value()) {
                return std::unexpected(EvaluatorError{
                    .code         = EvaluatorErrorCode::InternalError,
                    .evaluator_id = std::string{evaluator_id()},
                    .detail       = cas_result.error().detail,
                });
            }
            swapped = *cas_result;
        }

        if (!swapped) {
            return EvaluatorResult{
                .evaluator_id = std::string{evaluator_id()},
                .verdict      = Verdict::Pass,
                .reason_code  = "cas_contention",
            };
        }

        const bool    exceeded = updated.aggregate > config_.threshold;
        const Verdict verdict  = exceeded ? Verdict::Flag : Verdict::Pass;

        EvaluatorResult result{
            .evaluator_id = std::string{evaluator_id()},
            .verdict      = verdict,
        };
        if (exceeded) {
            result.reason_code = "threshold_exceeded";
            result.metadata    = std::format(
                R"({{"window_count":{},"threshold":{},"window_name":"{}"}})",
                static_cast<int>(updated.aggregate),
                config_.threshold,
                config_.window_name);
        }
        return result;
    }

    static constexpr std::string_view evaluator_id() noexcept {
        return "threshold_evaluator";
    }

private:
    ThresholdEvaluatorConfig config_;
    std::shared_ptr<Store>   store_;

    [[nodiscard]] std::string entity_key(const Event& event) const {
        switch (config_.group_by) {
            case GroupBy::EntityId:  return std::string{event.entity_id};
            case GroupBy::TenantId:  return std::string{event.tenant_id};
            case GroupBy::Tag:       return std::string{event.tag(config_.group_by_tag)};
        }
        return std::string{event.entity_id};
    }

    [[nodiscard]] uint64_t current_epoch(const Event& event) const noexcept {
        const auto ts_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                event.timestamp.time_since_epoch()).count());
        const auto window_ms = static_cast<uint64_t>(config_.window_duration.count());
        return window_ms == 0 ? 0 : ts_ms / window_ms;
    }

    [[nodiscard]] double aggregate_value(double current, const Event& /*event*/) const noexcept {
        switch (config_.aggregation) {
            case AggregationFn::Count:         return current + 1.0;
            case AggregationFn::Sum:           return current + 1.0;
            case AggregationFn::DistinctCount: return current + 1.0;
        }
        return current + 1.0;
    }
};

static_assert(LightweightEvaluator<ThresholdEvaluator<InProcessWindowStore>>);

}  // namespace fre
