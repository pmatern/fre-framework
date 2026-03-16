/// T027 — Concept satisfaction tests for all evaluator contracts.
/// These compile-time assertions verify that synthetic types satisfy the
/// C++23 Concepts defined in include/fre/core/concepts.hpp.

#include <fre/core/concepts.hpp>
#include <fre/core/decision.hpp>
#include <fre/core/error.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>
#include <fre/state/window_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <span>
#include <vector>

using namespace fre;

// ─── Synthetic types satisfying each concept ─────────────────────────────────

struct SyntheticPassEval {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "synthetic_pass", .verdict = Verdict::Pass};
    }
};

struct SyntheticFlagEval {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "synthetic_flag", .verdict = Verdict::Flag};
    }
};

struct SyntheticBlockEval {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "synthetic_block", .verdict = Verdict::Block};
    }
};

struct SyntheticInferenceEval {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{
            .evaluator_id = "synthetic_inference",
            .verdict      = Verdict::Flag,
            .score        = 0.85f,
        };
    }
};

struct SyntheticBatchInferenceEval {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& ev) {
        return SyntheticInferenceEval{}.evaluate(ev);
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

struct SyntheticPolicyEval {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const PolicyContext& /*ctx*/) {
        return EvaluatorResult{.evaluator_id = "synthetic_policy", .verdict = Verdict::Pass};
    }
};

struct SyntheticEmitTarget {
    std::expected<void, EmissionError> emit(Decision /*d*/) { return {}; }
};

// ─── InMemory StateStore ─────────────────────────────────────────────────────

struct SyntheticStateStore {
    std::expected<WindowValue, StoreError> get(const WindowKey& /*key*/) {
        return WindowValue{};
    }
    std::expected<bool, StoreError> compare_and_swap(
        const WindowKey& /*key*/, const WindowValue& /*old_val*/, const WindowValue& /*new_val*/) {
        return true;
    }
    std::expected<void, StoreError> expire(const WindowKey& /*key*/) { return {}; }
    bool is_available() const noexcept { return true; }
};

// ─── Concept static_assert (compile-time verification) ───────────────────────

static_assert(LightweightEvaluator<SyntheticPassEval>);
static_assert(LightweightEvaluator<SyntheticFlagEval>);
static_assert(LightweightEvaluator<SyntheticBlockEval>);

static_assert(InferenceEvaluator<SyntheticInferenceEval>);
static_assert(InferenceEvaluator<SyntheticBatchInferenceEval>);

static_assert(PolicyEvaluator<SyntheticPolicyEval>);

static_assert(EmissionTarget<SyntheticEmitTarget>);

static_assert(StateStore<SyntheticStateStore>);

// ─── Dispatch tag helpers ─────────────────────────────────────────────────────

static_assert(!has_evaluate_batch<SyntheticInferenceEval>);
static_assert(has_evaluate_batch<SyntheticBatchInferenceEval>);

static_assert(!has_flush<SyntheticEmitTarget>);

// ─── Runtime tests (concept satisfaction is already checked above) ────────────

TEST_CASE("Concept types compile and run correctly", "[contract][concepts][US2]") {
    Event ev{
        .tenant_id  = "t",
        .entity_id  = "e",
        .event_type = "test",
        .timestamp  = std::chrono::system_clock::now(),
    };

    SECTION("LightweightEvaluator") {
        SyntheticPassEval  pass;
        SyntheticBlockEval block;

        REQUIRE(pass.evaluate(ev)->verdict  == Verdict::Pass);
        REQUIRE(block.evaluate(ev)->verdict == Verdict::Block);
    }

    SECTION("InferenceEvaluator — single event") {
        SyntheticInferenceEval inf;
        auto r = inf.evaluate(ev);
        REQUIRE(r.has_value());
        REQUIRE(r->score.has_value());
        REQUIRE(*r->score > 0.0f);
    }

    SECTION("InferenceEvaluator — batch dispatch") {
        SyntheticBatchInferenceEval batch_inf;
        std::vector<const Event*> ptrs = {&ev, &ev, &ev};
        auto r = batch_inf.evaluate_batch(std::span<const Event* const>{ptrs});
        REQUIRE(r.has_value());
        REQUIRE(r->size() == 3);
    }

    SECTION("PolicyEvaluator") {
        const StageOutput stage_out{.stage_id = "eval", .verdict = Verdict::Flag};
        const PolicyContext ctx{ev, std::span<const StageOutput>{&stage_out, 1}};
        SyntheticPolicyEval policy;
        REQUIRE(policy.evaluate(ctx)->verdict == Verdict::Pass);
    }

    SECTION("EmissionTarget") {
        SyntheticEmitTarget target;
        Decision d;
        d.event_id = 1;
        REQUIRE(target.emit(std::move(d)).has_value());
    }
}
