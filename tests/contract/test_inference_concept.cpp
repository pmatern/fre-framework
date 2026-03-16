/// T044 — Concept behaviour test: InferenceEvaluator with optional evaluate_batch().

#include <fre/core/concepts.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <vector>

using namespace fre;
using namespace std::chrono_literals;

// ─── Synthetic inference evaluators ──────────────────────────────────────────

/// Single-event inference evaluator (no batch method).
struct SyntheticSingleEval {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{
            .evaluator_id = "single_inf",
            .verdict      = Verdict::Pass,
            .score        = 0.3f,
        };
    }
};
static_assert(InferenceEvaluator<SyntheticSingleEval>);
static_assert(!has_evaluate_batch<SyntheticSingleEval>);

/// Batch-capable inference evaluator.
struct SyntheticBatchEval {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{
            .evaluator_id = "batch_inf",
            .verdict      = Verdict::Flag,
            .score        = 0.9f,
        };
    }

    std::expected<std::vector<EvaluatorResult>, EvaluatorError>
    evaluate_batch(std::span<const Event* const> events) {
        std::vector<EvaluatorResult> results;
        results.reserve(events.size());
        for (const auto* ev : events) {
            auto r = evaluate(*ev);
            if (r.has_value()) results.push_back(std::move(*r));
        }
        return results;
    }
};
static_assert(InferenceEvaluator<SyntheticBatchEval>);
static_assert(has_evaluate_batch<SyntheticBatchEval>);

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("InferenceEvaluator: single-event path", "[contract][inference][US4]") {
    SyntheticSingleEval eval;
    Event ev{
        .tenant_id  = "t",
        .entity_id  = "e",
        .event_type = "test",
        .timestamp  = std::chrono::system_clock::now(),
    };

    auto result = eval.evaluate(ev);
    REQUIRE(result.has_value());
    REQUIRE(result->score.has_value());
    REQUIRE(*result->score < 0.5f);
}

TEST_CASE("InferenceEvaluator: batch dispatch invoked when available", "[contract][inference][US4]") {
    SyntheticBatchEval eval;
    Event ev{
        .tenant_id  = "t",
        .entity_id  = "e",
        .event_type = "test",
        .timestamp  = std::chrono::system_clock::now(),
    };

    std::vector<const Event*> ptrs = {&ev, &ev, &ev};
    auto batch_result = eval.evaluate_batch(std::span<const Event* const>{ptrs});
    REQUIRE(batch_result.has_value());
    REQUIRE(batch_result->size() == 3);
    for (const auto& r : *batch_result) {
        REQUIRE(r.score.has_value());
        REQUIRE(*r.score > 0.5f);
    }
}

TEST_CASE("InferenceEvaluator: has_evaluate_batch distinguishes batch-capable types", "[contract][inference][US4]") {
    STATIC_CHECK(!has_evaluate_batch<SyntheticSingleEval>);
    STATIC_CHECK(has_evaluate_batch<SyntheticBatchEval>);
}
