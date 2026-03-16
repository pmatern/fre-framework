#pragma once

#include <fre/core/decision.hpp>
#include <fre/core/error.hpp>
#include <fre/core/event.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/pipeline/sync_submit.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>

namespace fre {

// ─── Pipeline lifecycle states ────────────────────────────────────────────────

enum class PipelineState : uint8_t {
    Stopped,
    Starting,
    Running,
    Draining,
};

// ─── Pipeline ────────────────────────────────────────────────────────────────

/// The top-level pipeline assembly. Owns all stage instances and the TenantRouter.
///
/// Lifecycle: Stopped → Starting → Running → Draining → Stopped
///
/// Thread safety: submit() is thread-safe and may be called concurrently from
/// multiple threads. start(), drain(), and stop() must not be called concurrently
/// with each other.
class Pipeline {
public:
    explicit Pipeline(PipelineConfig config);
    ~Pipeline();

    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&)                 = delete;
    Pipeline& operator=(Pipeline&&)      = delete;

    /// Validate config, allocate stages, start the coroutine executor.
    /// Returns PipelineError if already running or if config validation fails.
    [[nodiscard]] std::expected<void, Error> start();

    /// Submit one event for evaluation. Thread-safe.
    /// Returns RateLimitError if the tenant's budget is exhausted.
    /// Returns PipelineError if the pipeline is not running.
    [[nodiscard]] std::expected<void, Error> submit(Event event);

    /// Submit one event and block until the pipeline produces a Decision.
    /// Respects the pipeline's latency_budget as a maximum timeout.
    /// On success, registered emission targets also fire (same as async submit).
    /// On any error, no decision is emitted to targets.
    /// Thread-safe; safe to call concurrently from multiple threads.
    /// @param cancel  Optional stop token. Default {} = no cancellation.
    [[nodiscard]] std::expected<Decision, SubmitSyncError>
    submit_sync(Event event, StopToken cancel = {});

    /// Stop accepting new events and wait up to `deadline` for in-flight
    /// evaluations to complete. Transitions to Stopped.
    void drain(std::chrono::milliseconds deadline = std::chrono::seconds{30});

    /// Returns the current lifecycle state.
    [[nodiscard]] PipelineState state() const noexcept;

    /// Returns the pipeline_id from the config.
    [[nodiscard]] std::string_view pipeline_id() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fre
