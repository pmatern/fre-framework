#include <fre/evaluator/onnx_inference_evaluator.hpp>

#ifdef FRE_ENABLE_ONNX

#include <fre/core/logging.hpp>

#include <mutex>
#include <stdexcept>

namespace fre {

// ─── Process-global Ort::Env singleton ───────────────────────────────────────
//
// ORT documentation states that Ort::Env must be created before any session and
// must outlive all sessions. A single global Env with ORT_LOGGING_LEVEL_WARNING
// satisfies that requirement for all evaluator instances in the process.

namespace {

Ort::Env& global_ort_env() {
    // Constructed at first call; destroyed at program exit.
    // Ort::Env constructor is not thread-safe, but the mutex below ensures
    // it is called exactly once.
    static std::once_flag  flag;
    static Ort::Env*       env_ptr = nullptr;

    std::call_once(flag, [] {
        // Leaking intentionally: ORT requires the Env to outlive all sessions,
        // including those in static storage. Deleting it in a destructor risks
        // destruction-order issues.
        env_ptr = new Ort::Env{ORT_LOGGING_LEVEL_WARNING, "fre"};  // NOLINT(cppcoreguidelines-owning-memory)
    });

    return *env_ptr;
}

}  // namespace

// ─── Constructor ─────────────────────────────────────────────────────────────

OnnxInferenceEvaluator::OnnxInferenceEvaluator(OnnxInferenceEvaluatorConfig config)
    : config_{std::move(config)}
    , memory_info_{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)}
{
    Ort::SessionOptions session_opts;
    session_opts.SetIntraOpNumThreads(config_.intra_op_num_threads);
    session_opts.SetInterOpNumThreads(config_.inter_op_num_threads);
    // Graph optimization: apply all static graph transforms.
    session_opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

    const std::string model_path_str = config_.model_path.string();

    session_ = std::make_unique<Ort::Session>(
        global_ort_env(),
        model_path_str.c_str(),
        session_opts);

    // Cache input / output node names (allocated by ORT allocator; copy to std::string).
    Ort::AllocatorWithDefaultOptions allocator;

    const std::size_t num_inputs = session_->GetInputCount();
    input_names_storage_.reserve(num_inputs);
    input_names_.reserve(num_inputs);
    for (std::size_t i = 0; i < num_inputs; ++i) {
        input_names_storage_.emplace_back(
            session_->GetInputNameAllocated(i, allocator).get());
        input_names_.push_back(input_names_storage_.back().c_str());
    }

    const std::size_t num_outputs = session_->GetOutputCount();
    output_names_storage_.reserve(num_outputs);
    output_names_.reserve(num_outputs);
    for (std::size_t i = 0; i < num_outputs; ++i) {
        output_names_storage_.emplace_back(
            session_->GetOutputNameAllocated(i, allocator).get());
        output_names_.push_back(output_names_storage_.back().c_str());
    }

    if (config_.score_output_index >= num_outputs) {
        throw std::invalid_argument{
            "OnnxInferenceEvaluator: score_output_index " +
            std::to_string(config_.score_output_index) +
            " is out of range (model has " + std::to_string(num_outputs) + " outputs)"};
    }

    FRE_LOG_INFO("OnnxInferenceEvaluator '{}' loaded model '{}' ({} inputs, {} outputs)",
                 config_.evaluator_id, model_path_str, num_inputs, num_outputs);
}

// ─── run_single ──────────────────────────────────────────────────────────────

std::expected<EvaluatorResult, EvaluatorError>
OnnxInferenceEvaluator::run_single(const Event& event) {
    if (!config_.input_extractor) {
        return std::unexpected(EvaluatorError{
            EvaluatorErrorCode::InternalError,
            std::string{config_.evaluator_id},
            "no input_extractor configured"});
    }

    // Per-invocation run options (allows per-call timeout / cancellation).
    Ort::RunOptions run_opts;

    // IoBinding: binds named input tensors produced by the extractor.
    Ort::IoBinding binding{*session_};

    // Let the caller-supplied extractor populate the binding's inputs.
    auto extractor_result = config_.input_extractor(event, binding);
    if (!extractor_result.has_value()) {
        return std::unexpected(std::move(extractor_result.error()));
    }

    // Run the model.
    OrtStatus* status_ptr = nullptr;
    try {
        session_->Run(run_opts, binding);
    } catch (const Ort::Exception& ex) {
        return std::unexpected(EvaluatorError{
            EvaluatorErrorCode::InternalError,
            std::string{config_.evaluator_id},
            ex.what()});
    }
    (void)status_ptr;

    // Extract the score from the designated output tensor.
    std::vector<Ort::Value> output_values = binding.GetOutputValues();
    if (config_.score_output_index >= output_values.size()) {
        return std::unexpected(EvaluatorError{
            EvaluatorErrorCode::InternalError,
            std::string{config_.evaluator_id},
            "output tensor index out of range after Run()"});
    }

    const auto& score_tensor = output_values[config_.score_output_index];
    const float* data = score_tensor.GetTensorData<float>();
    if (data == nullptr) {
        return std::unexpected(EvaluatorError{
            EvaluatorErrorCode::InternalError,
            std::string{config_.evaluator_id},
            "output tensor data pointer is null"});
    }
    const float score = data[0];

    EvaluatorResult result;
    result.evaluator_id = std::string{config_.evaluator_id};
    result.score        = score;
    // Verdict is left as Pass; InferenceStage applies score_threshold for final verdict.
    result.verdict      = Verdict::Pass;

    return result;
}

// ─── evaluate ────────────────────────────────────────────────────────────────

std::expected<EvaluatorResult, EvaluatorError>
OnnxInferenceEvaluator::evaluate(const Event& event) {
    return run_single(event);
}

// ─── evaluate_batch ──────────────────────────────────────────────────────────

std::vector<std::expected<EvaluatorResult, EvaluatorError>>
OnnxInferenceEvaluator::evaluate_batch(std::span<const Event* const> events) {
    std::vector<std::expected<EvaluatorResult, EvaluatorError>> results;
    results.reserve(events.size());

    for (const Event* ev : events) {
        if (ev == nullptr) {
            results.push_back(std::unexpected(EvaluatorError{
                EvaluatorErrorCode::InternalError,
                std::string{config_.evaluator_id},
                "null event pointer in batch"}));
        } else {
            results.push_back(run_single(*ev));
        }
    }

    return results;
}

}  // namespace fre

#endif  // FRE_ENABLE_ONNX
