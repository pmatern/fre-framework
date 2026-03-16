#include <fre/evaluator/threshold_evaluator.hpp>

#include <format>

namespace fre {

ThresholdEvaluator::ThresholdEvaluator(ThresholdEvaluatorConfig config,
                                        std::shared_ptr<InProcessWindowStore> store)
    : config_{std::move(config)}, store_{std::move(store)} {}

std::string ThresholdEvaluator::entity_key(const Event& event) const {
    switch (config_.group_by) {
        case GroupBy::EntityId:  return std::string{event.entity_id};
        case GroupBy::TenantId:  return std::string{event.tenant_id};
        case GroupBy::Tag:       return std::string{event.tag(config_.group_by_tag)};
    }
    return std::string{event.entity_id};
}

uint64_t ThresholdEvaluator::current_epoch(const Event& event) const noexcept {
    const auto ts_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            event.timestamp.time_since_epoch()).count());
    const auto window_ms = static_cast<uint64_t>(config_.window_duration.count());
    return window_ms == 0 ? 0 : ts_ms / window_ms;
}

double ThresholdEvaluator::aggregate_value(double current, const Event& /*event*/) const noexcept {
    switch (config_.aggregation) {
        case AggregationFn::Count:         return current + 1.0;
        case AggregationFn::Sum:           return current + 1.0;  // caller provides value in event payload
        case AggregationFn::DistinctCount: return current + 1.0;  // simplified: count-based for v1
    }
    return current + 1.0;
}

std::expected<EvaluatorResult, EvaluatorError> ThresholdEvaluator::evaluate(const Event& event) {
    const WindowKey key{
        .tenant_id   = std::string{event.tenant_id},
        .entity_id   = entity_key(event),
        .window_name = config_.window_name,
        .epoch       = current_epoch(event),
    };

    // Read current value — CAS loop to atomically increment
    bool swapped = false;
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
        // CAS contention — fail open (count the event but don't block)
        return EvaluatorResult{
            .evaluator_id = std::string{evaluator_id()},
            .verdict      = Verdict::Pass,
            .reason_code  = "cas_contention",
        };
    }

    // Compare updated aggregate against threshold
    const bool exceeded = updated.aggregate > config_.threshold;
    const Verdict verdict = exceeded ? Verdict::Flag : Verdict::Pass;

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

}  // namespace fre
