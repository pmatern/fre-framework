#pragma once

// OnnxInferenceEvaluator is only available when the project is built with FRE_ENABLE_ONNX=ON.
// Consumers that include this header without defining FRE_ENABLE_ONNX will receive a
// static_assert at compile time.

#ifndef FRE_ENABLE_ONNX
static_assert(false,
    "OnnxInferenceEvaluator requires FRE_ENABLE_ONNX=ON and the ONNX Runtime library. "
    "Pass -DFRE_ENABLE_ONNX=ON to CMake.");
#endif

#include <fre/core/concepts.hpp>
#include <fre/core/error.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>

#include <onnxruntime/core/session/onnxruntime_cxx_api.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fre {

// ─── OnnxInferenceEvaluatorConfig ────────────────────────────────────────────

struct OnnxInferenceEvaluatorConfig {
    /// Path to the ONNX model file.
    std::filesystem::path model_path;

    /// Human-readable evaluator ID (used in EvaluatorResult::evaluator_id).
    std::string evaluator_id{"onnx_evaluator"};

    /// Inter-op and intra-op thread counts passed to SessionOptions.
    /// 0 = use ORT default (typically hardware concurrency).
    int intra_op_num_threads{0};
    int inter_op_num_threads{0};

    /// Callback: given an event, populate the input tensor values for one inference call.
    /// The callback receives pre-allocated IoBinding that must have all model inputs bound.
    /// If null, evaluate() returns EvaluatorError::InternalError("no input_extractor configured").
    using InputExtractorFn = std::function<
        std::expected<void, EvaluatorError>(const Event&, Ort::IoBinding&)>;
    InputExtractorFn input_extractor;

    /// Index of the output node whose float[0] value is used as the anomaly score.
    std::size_t score_output_index{0};
};

// ─── OnnxInferenceEvaluator ───────────────────────────────────────────────────

/// Satisfies InferenceEvaluator<OnnxInferenceEvaluator>.
///
/// Thread safety:
///   - evaluate() may be called concurrently from multiple threads; each call
///     creates its own Ort::RunOptions and Ort::IoBinding.
///   - evaluate_batch() follows the same guarantee.
///   - Construction and destruction must be externally serialised (not concurrent).
class OnnxInferenceEvaluator {
public:
    explicit OnnxInferenceEvaluator(OnnxInferenceEvaluatorConfig config);

    // Non-copyable (Ort::Session is move-only).
    OnnxInferenceEvaluator(const OnnxInferenceEvaluator&)            = delete;
    OnnxInferenceEvaluator& operator=(const OnnxInferenceEvaluator&) = delete;
    OnnxInferenceEvaluator(OnnxInferenceEvaluator&&)                  = default;
    OnnxInferenceEvaluator& operator=(OnnxInferenceEvaluator&&)       = default;
    ~OnnxInferenceEvaluator()                                         = default;

    [[nodiscard]] std::string_view evaluator_id() const noexcept {
        return config_.evaluator_id;
    }

    /// Evaluate a single event. Satisfies InferenceEvaluator::evaluate().
    [[nodiscard]] std::expected<EvaluatorResult, EvaluatorError>
    evaluate(const Event& event);

    /// Batch evaluation. Satisfies optional InferenceEvaluator::evaluate_batch().
    /// Returns one EvaluatorResult per event in the span, in order.
    [[nodiscard]] std::vector<std::expected<EvaluatorResult, EvaluatorError>>
    evaluate_batch(std::span<const Event* const> events);

private:
    [[nodiscard]] std::expected<EvaluatorResult, EvaluatorError>
    run_single(const Event& event);

    OnnxInferenceEvaluatorConfig config_;

    // The Ort::Env and Ort::Session are held via unique_ptr so that this type
    // remains movable even though ORT objects are non-copyable.
    std::unique_ptr<Ort::Env>     env_;    // process-global; shared via singleton helper
    std::unique_ptr<Ort::Session> session_;

    // Input/output node name caches (retrieved once at construction).
    std::vector<std::string>        input_names_storage_;
    std::vector<std::string>        output_names_storage_;
    std::vector<const char*>        input_names_;
    std::vector<const char*>        output_names_;
    Ort::MemoryInfo                 memory_info_;
};

// Compile-time concept check — caught at instantiation site.
static_assert(InferenceEvaluator<OnnxInferenceEvaluator>,
    "OnnxInferenceEvaluator must satisfy fre::InferenceEvaluator");

}  // namespace fre
