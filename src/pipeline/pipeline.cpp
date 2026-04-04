#include <fre/pipeline/pipeline.hpp>
#include <fre/core/logging.hpp>
#include <fre/pipeline/sync_submit.hpp>
#include <fre/sharding/tenant_router.hpp>
#include <fre/stage/emit_stage.hpp>
#include <fre/stage/eval_stage.hpp>
#include <fre/stage/ingest_stage.hpp>
#include <fre/stage/inference_stage.hpp>
#include <fre/stage/policy_stage.hpp>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace fre {

using namespace std::chrono_literals;

// ─── SyncContext ─────────────────────────────────────────────────────────────
// Translation-unit private. One instance per submit_sync() call.
// Shared between the calling thread (via submit_sync) and the run_event coroutine.

struct SyncContext {
    std::mutex                         mtx;
    std::condition_variable_any        cv;
    std::optional<Decision>            decision;
    std::optional<SubmitSyncError>     error;
    std::atomic<bool>                  cancelled{false};
    bool                               done{false};
};

// ─── Pipeline::Impl ──────────────────────────────────────────────────────────

struct Pipeline::Impl {
    PipelineConfig config;
    std::atomic<PipelineState> state_{PipelineState::Stopped};
    std::atomic<uint64_t>      next_event_id_{1};
    std::atomic<int64_t>       in_flight_{0};
    std::atomic<bool>          ever_started_{false};

    std::unique_ptr<TenantRouter>   router;
    std::unique_ptr<IngestStage>    ingest;
    std::unique_ptr<EvalStage>      eval;
    std::unique_ptr<InferenceStage> inference;
    std::unique_ptr<PolicyStage>    policy;
    std::unique_ptr<EmitStage>      emit;

    std::mutex              drain_mutex;
    std::condition_variable drain_cv;

    explicit Impl(PipelineConfig cfg) : config{std::move(cfg)} {}

    void on_event_complete() {
        if (in_flight_.fetch_sub(1, std::memory_order_release) == 1) {
            std::lock_guard lock{drain_mutex};
            drain_cv.notify_all();
        }
    }

    asio::awaitable<void> run_event(Event event, std::shared_ptr<SyncContext> sync_ctx = nullptr) {
        const auto receipt_time = std::chrono::steady_clock::now();
        const uint64_t event_id = next_event_id_.fetch_add(1, std::memory_order_relaxed);
        const std::string tenant_id{event.tenant_id};

        Decision decision;
        decision.pipeline_id      = config.pipeline_id;
        decision.pipeline_version = config.pipeline_version;
        decision.event_id         = event_id;
        decision.tenant_id        = tenant_id;
        decision.entity_id        = std::string{event.entity_id};
        decision.decided_at       = std::chrono::system_clock::now();

        // Assign stable event id
        event.id = event_id;

        // ─── Ingest stage ────────────────────────────────────────────────────
        auto ingest_out = ingest->process(event);
        if (ingest_out.has_value()) {
            // Check validation failure before moving into decision (for sync path)
            const bool validation_failed =
                (ingest_out->degraded_reason == DegradedReason::IngestValidationFailed);
            decision.stage_outputs.push_back(std::move(*ingest_out));

            if (sync_ctx && validation_failed) {
                {
                    std::lock_guard lk{sync_ctx->mtx};
                    sync_ctx->error = SubmitSyncError::ValidationFailed;
                    sync_ctx->done  = true;
                }
                sync_ctx->cv.notify_one();
                on_event_complete();
                co_return;
            }
        }

        // ─── Eval stage (optional) ────────────────────────────────────────────
        if (eval) {
            auto eval_out = eval->process(event);
            if (eval_out.has_value()) {
                decision.stage_outputs.push_back(std::move(*eval_out));
            }
        }

        // ─── Inference stage (optional) ───────────────────────────────────────
        if (inference) {
            auto inf_out = inference->process(event);
            if (inf_out.has_value()) {
                decision.stage_outputs.push_back(std::move(*inf_out));
            }
        }

        // ─── Policy stage (optional) ──────────────────────────────────────────
        if (policy) {
            PolicyContext ctx{
                .event         = event,
                .stage_outputs = std::span<const StageOutput>{decision.stage_outputs},
            };
            auto policy_out = policy->process(ctx);
            if (policy_out.has_value()) {
                decision.stage_outputs.push_back(std::move(*policy_out));
            }
        }

        // ─── Compute final verdict and degradation ────────────────────────────
        decision.compute_final_verdict();
        decision.merge_degraded_reasons();

        // ─── Resolve active decisions (multi-decision mode) ───────────────────
        if (config.decision_type_registry.has_value()) {
            decision.compute_active_decisions(&*config.decision_type_registry);
        }

        // ─── Elapsed time ─────────────────────────────────────────────────────
        const auto elapsed = std::chrono::steady_clock::now() - receipt_time;
        decision.elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());

        // ─── Audit log ────────────────────────────────────────────────────────
        log_audit(decision);

        // ─── Sync path: check cancellation and capture decision ────────────────
        bool skip_emit = false;
        if (sync_ctx) {
            std::lock_guard lk{sync_ctx->mtx};
            if (sync_ctx->cancelled.load(std::memory_order_relaxed)) {
                skip_emit = true;
            } else {
                sync_ctx->decision = decision;  // copy before move-into-emit
            }
        }

        if (skip_emit) {
            sync_ctx->cv.notify_one();  // harmless; caller already moved on
            on_event_complete();
            co_return;
        }

        // ─── Emit stage ───────────────────────────────────────────────────────
        auto emit_out = emit->process(std::move(decision));
        // emit stage result is informational only; errors already handled inside EmitStage

        // ─── Notify sync caller (after emit, so emission happens first) ───────
        if (sync_ctx) {
            {
                std::lock_guard lk{sync_ctx->mtx};
                sync_ctx->done = true;
            }
            sync_ctx->cv.notify_one();
        }

        on_event_complete();
        co_return;
    }
};

// ─── Pipeline public API ──────────────────────────────────────────────────────

Pipeline::Pipeline(PipelineConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))} {}

Pipeline::~Pipeline() {
    if (impl_->state_.load(std::memory_order_acquire) == PipelineState::Running) {
        drain(5s);
    }
}

std::expected<void, Error> Pipeline::start() {
    PipelineState expected_stopped = PipelineState::Stopped;
    if (!impl_->state_.compare_exchange_strong(
            expected_stopped, PipelineState::Starting,
            std::memory_order_acq_rel)) {
        return std::unexpected(Error{PipelineError{
            PipelineErrorCode::AlreadyRunning, "pipeline is not stopped"}});
    }

    // Allocate stages
    impl_->ingest = std::make_unique<IngestStage>(impl_->config.ingest_config);

    if (impl_->config.eval_config) {
        impl_->eval = std::make_unique<EvalStage>(*impl_->config.eval_config);
    }

    if (impl_->config.inference_config) {
        impl_->inference = std::make_unique<InferenceStage>(*impl_->config.inference_config);
    }

    if (impl_->config.policy_config) {
        // Pass registry pointer when present. The registry is owned by impl_->config
        // and is stable for the pipeline lifetime — do not move config after start().
        const DecisionTypeRegistry* registry =
            impl_->config.decision_type_registry.has_value()
                ? &*impl_->config.decision_type_registry
                : nullptr;
        impl_->policy = std::make_unique<PolicyStage>(
            *impl_->config.policy_config, registry);
    }

    impl_->emit = std::make_unique<EmitStage>(impl_->config.emit_config);

    // Allocate router
    impl_->router = std::make_unique<TenantRouter>(
        impl_->config.sharding,
        impl_->config.rate_limit);

    impl_->ever_started_.store(true, std::memory_order_release);
    impl_->state_.store(PipelineState::Running, std::memory_order_release);
    FRE_LOG_INFO("Pipeline '{}' started", impl_->config.pipeline_id);
    return {};
}

std::expected<void, Error> Pipeline::submit(Event event) {
    if (impl_->state_.load(std::memory_order_acquire) != PipelineState::Running) {
        return std::unexpected(Error{PipelineError{
            PipelineErrorCode::NotRunning, "pipeline must be running to submit events"}});
    }

    // Rate limit check
    auto acquire = impl_->router->try_acquire(event.tenant_id);
    if (!acquire) {
        return std::unexpected(Error{std::move(acquire.error())});
    }

    impl_->in_flight_.fetch_add(1, std::memory_order_acquire);

    // Dispatch to one of the tenant's assigned strands
    auto strands = impl_->router->cells_for(event.tenant_id);
    auto& strand = *strands[0];  // simple: use first assigned strand
    const std::string tenant_id_copy{event.tenant_id};

    asio::co_spawn(strand,
        impl_->run_event(std::move(event)),
        asio::detached);

    impl_->router->release(tenant_id_copy);
    return {};
}

std::expected<Decision, SubmitSyncError>
Pipeline::submit_sync(Event event, StopToken cancel) {
    const auto receipt_time = std::chrono::steady_clock::now();

    // ─── Pre-call cancellation ────────────────────────────────────────────
    if (cancel.stop_requested()) {
        return std::unexpected(SubmitSyncError::Cancelled);
    }

    // ─── Pipeline state check ─────────────────────────────────────────────
    const auto cur_state = impl_->state_.load(std::memory_order_acquire);
    if (cur_state != PipelineState::Running) {
        if (!impl_->ever_started_.load(std::memory_order_acquire)) {
            return std::unexpected(SubmitSyncError::NotStarted);
        }
        return std::unexpected(SubmitSyncError::PipelineUnavailable);
    }

    // ─── Rate-limit check ─────────────────────────────────────────────────
    auto acquire = impl_->router->try_acquire(event.tenant_id);
    if (!acquire) {
        return std::unexpected(SubmitSyncError::RateLimited);
    }

    // ─── Allocate sync context and spawn coroutine ────────────────────────
    auto ctx = std::make_shared<SyncContext>();
    impl_->in_flight_.fetch_add(1, std::memory_order_acquire);

    auto strands = impl_->router->cells_for(event.tenant_id);
    auto& strand = *strands[0];
    const std::string tenant_id_copy{event.tenant_id};

    FRE_LOG_INFO("sync_submit: tenant={} entity={} event_type={}",
                 event.tenant_id, event.entity_id, event.event_type);

    asio::co_spawn(strand, impl_->run_event(std::move(event), ctx), asio::detached);
    impl_->router->release(tenant_id_copy);

    // ─── Block until decision, timeout, or cancellation ───────────────────
    // We poll in short slices to support cancellation when stop_token is provided.
    // When no stop_token is in use (stop_possible() == false), we do a single
    // wait_for for the full budget (no polling overhead).
    std::unique_lock lk{ctx->mtx};
    const auto budget = impl_->config.latency_budget;
    const auto deadline = std::chrono::steady_clock::now() + budget;

    bool timed_out_or_cancelled = false;
    bool cancelled_flag = false;

    if (!cancel.stop_possible()) {
        // No cancellation — single blocking wait
        const bool done_flag = ctx->cv.wait_for(lk, budget,
                                                [&ctx] { return ctx->done; });
        if (!done_flag) {
            timed_out_or_cancelled = true;
        }
    } else {
        // Poll in 5ms slices to check cancellation
        while (!ctx->done) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                timed_out_or_cancelled = true;
                break;
            }
            if (cancel.stop_requested()) {
                timed_out_or_cancelled = true;
                cancelled_flag = true;
                break;
            }
            const auto remaining = deadline - now;
            const auto slice = std::chrono::milliseconds{5};
            ctx->cv.wait_for(lk, remaining < slice ? remaining : slice,
                             [&ctx] { return ctx->done; });
        }
    }

    if (!timed_out_or_cancelled) {
        // Decision produced (or error like ValidationFailed)
        if (ctx->error.has_value()) {
            const auto elapsed_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - receipt_time).count());
            FRE_LOG_INFO("sync_submit: error={} elapsed_us={}",
                         static_cast<int>(*ctx->error), elapsed_us);
            return std::unexpected(*ctx->error);
        }
        const auto elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - receipt_time).count());
        FRE_LOG_INFO("sync_submit: decision produced elapsed_us={}", elapsed_us);
        return std::move(*ctx->decision);
    }

    // Timeout or cancellation: set cancelled flag so coroutine skips emit
    ctx->cancelled.store(true, std::memory_order_release);

    // Check if decision was captured just before we set cancelled
    if (ctx->decision.has_value()) {
        const auto elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - receipt_time).count());
        FRE_LOG_INFO("sync_submit: decision captured at timeout boundary elapsed_us={}", elapsed_us);
        return std::move(*ctx->decision);
    }

    lk.unlock();

    if (cancelled_flag || cancel.stop_requested()) {
        FRE_LOG_INFO("sync_submit: cancelled by caller");
        return std::unexpected(SubmitSyncError::Cancelled);
    }
    FRE_LOG_INFO("sync_submit: timeout after {}ms", budget.count());
    return std::unexpected(SubmitSyncError::Timeout);
}

void Pipeline::drain(std::chrono::milliseconds deadline) {
    impl_->state_.store(PipelineState::Draining, std::memory_order_release);

    std::unique_lock lock{impl_->drain_mutex};
    impl_->drain_cv.wait_for(lock, deadline, [this] {
        return impl_->in_flight_.load(std::memory_order_acquire) == 0;
    });

    impl_->router->stop();
    impl_->state_.store(PipelineState::Stopped, std::memory_order_release);
    FRE_LOG_INFO("Pipeline '{}' drained", impl_->config.pipeline_id);
}

PipelineState Pipeline::state() const noexcept {
    return impl_->state_.load(std::memory_order_acquire);
}

std::string_view Pipeline::pipeline_id() const noexcept {
    return impl_->config.pipeline_id;
}

}  // namespace fre
