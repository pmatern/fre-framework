#include <fre/core/error.hpp>

#include <format>

namespace fre {

std::string ConfigError::message() const {
    const char* code_str = [&] {
        switch (code) {
            case ConfigErrorCode::RequiredStageMissing:    return "RequiredStageMissing";
            case ConfigErrorCode::LatencyBudgetExceeded:   return "LatencyBudgetExceeded";
            case ConfigErrorCode::UndefinedStageDependency: return "UndefinedStageDependency";
            case ConfigErrorCode::InvalidShardingConfig:   return "InvalidShardingConfig";
            case ConfigErrorCode::EvaluatorLoadFailed:     return "EvaluatorLoadFailed";
            case ConfigErrorCode::InvalidWindowConfig:     return "InvalidWindowConfig";
            case ConfigErrorCode::InvalidPolicyRule:       return "InvalidPolicyRule";
            default:                                        return "Unknown";
        }
    }();
    return detail.empty() ? code_str : std::format("{}: {}", code_str, detail);
}

std::string EvaluatorError::message() const {
    const char* code_str = [&] {
        switch (code) {
            case EvaluatorErrorCode::Timeout:        return "Timeout";
            case EvaluatorErrorCode::InternalError:  return "InternalError";
            case EvaluatorErrorCode::InvalidInput:   return "InvalidInput";
            case EvaluatorErrorCode::NotInitialized: return "NotInitialized";
            default:                                  return "Unknown";
        }
    }();
    if (evaluator_id.empty()) {
        return detail.empty() ? code_str : std::format("{}: {}", code_str, detail);
    }
    return detail.empty()
        ? std::format("{}[{}]", code_str, evaluator_id)
        : std::format("{}[{}]: {}", code_str, evaluator_id, detail);
}

std::string StoreError::message() const {
    const char* code_str = [&] {
        switch (code) {
            case StoreErrorCode::Unavailable:         return "Unavailable";
            case StoreErrorCode::Timeout:             return "Timeout";
            case StoreErrorCode::CasFailed:           return "CasFailed";
            case StoreErrorCode::SerializationError:  return "SerializationError";
            default:                                   return "Unknown";
        }
    }();
    return detail.empty() ? code_str : std::format("{}: {}", code_str, detail);
}

std::string EmissionError::message() const {
    const char* code_str = [&] {
        switch (code) {
            case EmissionErrorCode::TargetUnavailable:   return "TargetUnavailable";
            case EmissionErrorCode::Timeout:             return "Timeout";
            case EmissionErrorCode::BufferFull:          return "BufferFull";
            case EmissionErrorCode::SerializationError:  return "SerializationError";
            default:                                      return "Unknown";
        }
    }();
    return target_id.empty()
        ? std::format("{}: {}", code_str, detail)
        : std::format("{}[{}]: {}", code_str, target_id, detail);
}

std::string RateLimitError::message() const {
    const char* code_str = [&] {
        switch (code) {
            case RateLimitErrorCode::Exhausted:            return "RateLimitExhausted";
            case RateLimitErrorCode::ConcurrencyCapReached: return "ConcurrencyCapReached";
            default:                                         return "Unknown";
        }
    }();
    return tenant_id.empty() ? code_str : std::format("{}[{}]", code_str, tenant_id);
}

std::string PipelineError::message() const {
    const char* code_str = [&] {
        switch (code) {
            case PipelineErrorCode::NotRunning:    return "NotRunning";
            case PipelineErrorCode::AlreadyRunning: return "AlreadyRunning";
            case PipelineErrorCode::DrainTimeout:  return "DrainTimeout";
            case PipelineErrorCode::SubmitRejected: return "SubmitRejected";
            default:                                return "Unknown";
        }
    }();
    return detail.empty() ? code_str : std::format("{}: {}", code_str, detail);
}

}  // namespace fre
