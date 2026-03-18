#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace fre {

// ─── ConfigError ─────────────────────────────────────────────────────────────

enum class ConfigErrorCode : uint8_t {
    RequiredStageMissing,
    LatencyBudgetExceeded,
    UndefinedStageDependency,
    InvalidShardingConfig,
    EvaluatorLoadFailed,
    InvalidWindowConfig,
    InvalidPolicyRule,
    Unknown,
};

struct ConfigError {
    ConfigErrorCode code;
    std::string     detail;

    [[nodiscard]] std::string message() const;
};

// ─── EvaluatorError ──────────────────────────────────────────────────────────

enum class EvaluatorErrorCode : uint8_t {
    Timeout,
    InternalError,
    InvalidInput,
    NotInitialized,
};

struct EvaluatorError {
    EvaluatorErrorCode code;
    std::string        evaluator_id;
    std::string        detail;

    [[nodiscard]] std::string message() const;
};

// ─── StoreError ──────────────────────────────────────────────────────────────

enum class StoreErrorCode : uint8_t {
    Unavailable,
    Timeout,
    CasFailed,
    SerializationError,
    QueryRangeError,
};

struct StoreError {
    StoreErrorCode code;
    std::string    detail;

    [[nodiscard]] std::string message() const;
};

// ─── EmissionError ───────────────────────────────────────────────────────────

enum class EmissionErrorCode : uint8_t {
    TargetUnavailable,
    Timeout,
    BufferFull,
    SerializationError,
};

struct EmissionError {
    EmissionErrorCode code;
    std::string       target_id;
    std::string       detail;

    [[nodiscard]] std::string message() const;
};

// ─── RateLimitError ──────────────────────────────────────────────────────────

enum class RateLimitErrorCode : uint8_t {
    Exhausted,
    ConcurrencyCapReached,
};

struct RateLimitError {
    RateLimitErrorCode code;
    std::string        tenant_id;

    [[nodiscard]] std::string message() const;
};

// ─── PipelineError ───────────────────────────────────────────────────────────

enum class PipelineErrorCode : uint8_t {
    NotRunning,
    AlreadyRunning,
    DrainTimeout,
    SubmitRejected,
};

struct PipelineError {
    PipelineErrorCode code;
    std::string       detail;

    [[nodiscard]] std::string message() const;
};

// ─── FleetRoutingError ───────────────────────────────────────────────────────

enum class FleetRoutingErrorCode : uint8_t {
    NotOwner,            ///< This instance is not in the tenant's owner set
    TopologyUnavailable, ///< Fleet topology is not configured
};

struct FleetRoutingError {
    FleetRoutingErrorCode code;
    std::string           tenant_id;
    std::string           redirect_hint; ///< Comma-separated owner addresses (may be empty)

    [[nodiscard]] std::string message() const;
};

// ─── Error variant ───────────────────────────────────────────────────────────

using Error = std::variant<
    ConfigError,
    EvaluatorError,
    StoreError,
    EmissionError,
    RateLimitError,
    PipelineError,
    FleetRoutingError>;

[[nodiscard]] inline std::string error_message(const Error& e) {
    return std::visit([](const auto& v) { return v.message(); }, e);
}

}  // namespace fre
