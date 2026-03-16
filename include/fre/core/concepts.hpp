#pragma once

#include <fre/core/decision.hpp>
#include <fre/core/error.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>

#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace fre {

// ─── PolicyContext ────────────────────────────────────────────────────────────

/// Passed to PolicyEvaluator; contains all preceding stage outputs for one event.
struct PolicyContext {
    const Event&                   event;
    std::span<const StageOutput>   stage_outputs;
};

// ─── Concept: LightweightEvaluator ───────────────────────────────────────────
//
// Implemented by:
//   - Built-in ThresholdEvaluator, AllowDenyEvaluator
//   - Any user-provided type bound to the lightweight evaluation stage
//
// Contract (from contracts/evaluator-contract.md):
//   evaluate(const Event&) -> std::expected<EvaluatorResult, EvaluatorError>

template <typename E>
concept LightweightEvaluator = requires(E e, const Event& ev) {
    { e.evaluate(ev) } -> std::same_as<std::expected<EvaluatorResult, EvaluatorError>>;
};

// ─── Concept: InferenceEvaluator ─────────────────────────────────────────────
//
// Primary operation: per-event score returning [0.0, 1.0] in EvaluatorResult::score.
// Optional batch operation: evaluate_batch(span<const Event*>) -> vector<EvaluatorResult>

template <typename E>
concept InferenceEvaluator = requires(E e, const Event& ev) {
    { e.evaluate(ev) } -> std::same_as<std::expected<EvaluatorResult, EvaluatorError>>;
} &&
// Optional but detected at compile time if present:
(
    !requires(E e, std::span<const Event* const> batch) { e.evaluate_batch(batch); } ||
    requires(E e, std::span<const Event* const> batch) {
        { e.evaluate_batch(batch) }
            -> std::same_as<std::expected<std::vector<EvaluatorResult>, EvaluatorError>>;
    }
);

// ─── Concept: PolicyEvaluator ────────────────────────────────────────────────
//
// Receives the full PolicyContext (accumulated stage outputs) for each event.

template <typename E>
concept PolicyEvaluator = requires(E e, const PolicyContext& ctx) {
    { e.evaluate(ctx) } -> std::same_as<std::expected<EvaluatorResult, EvaluatorError>>;
};

// ─── Concept: EmissionTarget ─────────────────────────────────────────────────
//
// Receives Decision records asynchronously from the emission stage.
// Optional flush() for graceful drain.

template <typename E>
concept EmissionTarget = requires(E e, Decision d) {
    { e.emit(std::move(d)) } -> std::same_as<std::expected<void, EmissionError>>;
};

// ─── Concept: StateStore ─────────────────────────────────────────────────────
//
// Contract (from contracts/state-store-contract.md):
//   get(key)                        -> expected<WindowValue, StoreError>
//   compare_and_swap(key, old, new) -> expected<bool, StoreError>
//   expire(key)                     -> expected<void, StoreError>
//   is_available()                  -> bool (noexcept)

// Forward declare types used in the concept — defined in state/window_store.hpp
struct WindowKey;
struct WindowValue;

template <typename S>
concept StateStore = requires(S s, const WindowKey& key,
                               const WindowValue& old_val, const WindowValue& new_val) {
    { s.get(key) }                                -> std::same_as<std::expected<WindowValue, StoreError>>;
    { s.compare_and_swap(key, old_val, new_val) } -> std::same_as<std::expected<bool, StoreError>>;
    { s.expire(key) }                             -> std::same_as<std::expected<void, StoreError>>;
    { s.is_available() } noexcept                 -> std::same_as<bool>;
};

// ─── Helpers: concept-based dispatch tags ────────────────────────────────────

/// True if E supports evaluate_batch().
template <typename E>
inline constexpr bool has_evaluate_batch =
    requires(E e, std::span<const Event* const> b) { e.evaluate_batch(b); };

/// True if E supports flush().
template <typename E>
inline constexpr bool has_flush =
    requires(E e) { e.flush(); };

// ─── CppEvaluatorAdapter ─────────────────────────────────────────────────────
//
// Wraps any type satisfying LightweightEvaluator, InferenceEvaluator, or
// PolicyEvaluator into a type-erased EvaluatorFn compatible with stage
// registration. For same-binary use; no ABI-stability guarantees.

template <LightweightEvaluator E>
class CppLightweightAdapter {
public:
    explicit CppLightweightAdapter(E impl) : impl_{std::move(impl)} {
        static_assert(LightweightEvaluator<E>);
    }

    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& event) {
        return impl_.evaluate(event);
    }

private:
    E impl_;
};

template <InferenceEvaluator E>
class CppInferenceAdapter {
public:
    explicit CppInferenceAdapter(E impl) : impl_{std::move(impl)} {
        static_assert(InferenceEvaluator<E>);
    }

    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& event) {
        return impl_.evaluate(event);
    }

    // Forward evaluate_batch if impl supports it
    std::expected<std::vector<EvaluatorResult>, EvaluatorError>
    evaluate_batch(std::span<const Event* const> events)
    requires has_evaluate_batch<E>
    {
        return impl_.evaluate_batch(events);
    }

private:
    E impl_;
};

}  // namespace fre
