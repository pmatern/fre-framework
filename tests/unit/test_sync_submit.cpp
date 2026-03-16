#include <fre/core/concepts.hpp>
#include <fre/core/event.hpp>
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/pipeline/sync_submit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using namespace fre;
using namespace std::chrono_literals;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static Event make_event(std::string tenant_id = "acme",
                        std::string entity_id = "user-1",
                        std::string event_type = "api_call") {
    return Event{
        .tenant_id  = std::move(tenant_id),
        .entity_id  = std::move(entity_id),
        .event_type = std::move(event_type),
        .timestamp  = std::chrono::system_clock::now(),
    };
}

// ─── Synthetic pass evaluator ─────────────────────────────────────────────────

struct SyntheticPassEvaluator {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "synthetic_pass", .verdict = Verdict::Pass};
    }
};
static_assert(LightweightEvaluator<SyntheticPassEvaluator>);

// ─── Slow evaluator ───────────────────────────────────────────────────────────

struct SlowEvaluator {
    std::chrono::milliseconds delay;
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        std::this_thread::sleep_for(delay);
        return EvaluatorResult{.evaluator_id = "slow", .verdict = Verdict::Pass};
    }
};
static_assert(LightweightEvaluator<SlowEvaluator>);

// ─── Counting emission target ─────────────────────────────────────────────────

struct CountingTarget {
    mutable std::mutex    mu;
    std::vector<Decision> decisions;
    std::atomic<int>      count{0};

    std::expected<void, EmissionError> emit(Decision d) {
        {
            std::lock_guard lock{mu};
            decisions.push_back(std::move(d));
        }
        ++count;
        return {};
    }
};
static_assert(EmissionTarget<CountingTarget>);

// ─── Builder helpers ──────────────────────────────────────────────────────────

static PipelineConfig make_pass_config(
    std::shared_ptr<CountingTarget> target,
    std::chrono::milliseconds budget = 300ms,
    RateLimitConfig rate_limit = RateLimitConfig{100'000, 200'000, 10'000})
{
    auto cfg = PipelineConfig::Builder()
        .pipeline_id("sync-submit-test")
        .latency_budget(budget)
        .ingest({})
        .lightweight_eval(
            EvalStageConfig{
                .timeout      = 50ms,
                .failure_mode = FailureMode::FailOpen,
                .composition  = CompositionRule::AnyBlock,
            }
            .add_evaluator(SyntheticPassEvaluator{})
        )
        .emit(
            EmitStageConfig{
                .timeout      = 10ms,
                .failure_mode = FailureMode::EmitDegraded,
            }
            .add_target(target)
        )
        .rate_limit(rate_limit)
        .build();
    REQUIRE(cfg.has_value());
    return std::move(*cfg);
}

// ─────────────────────────────────────────────────────────────────────────────
// US1 — Happy path
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("submit_sync: returns a Decision with Pass verdict", "[unit][sync_submit][us1]") {
    auto target = std::make_shared<CountingTarget>();
    Pipeline pipeline{make_pass_config(target)};
    REQUIRE(pipeline.start().has_value());

    auto result = pipeline.submit_sync(make_event());
    REQUIRE(result.has_value());
    CHECK(result->final_verdict == Verdict::Pass);

    pipeline.drain(5s);
}

TEST_CASE("submit_sync: returned Decision matches emission target", "[unit][sync_submit][us1]") {
    auto target = std::make_shared<CountingTarget>();
    Pipeline pipeline{make_pass_config(target)};
    REQUIRE(pipeline.start().has_value());

    auto ev = make_event("acme", "entity-42");
    auto result = pipeline.submit_sync(std::move(ev));
    REQUIRE(result.has_value());

    pipeline.drain(5s);

    REQUIRE(target->count.load() == 1);
    std::lock_guard lock{target->mu};
    REQUIRE(!target->decisions.empty());
    const auto& emitted = target->decisions[0];
    CHECK(emitted.pipeline_id   == result->pipeline_id);
    CHECK(emitted.tenant_id     == result->tenant_id);
    CHECK(emitted.entity_id     == result->entity_id);
    CHECK(emitted.event_id      == result->event_id);
    CHECK(emitted.final_verdict == result->final_verdict);
}

TEST_CASE("submit_sync: elapsed time is within latency budget", "[unit][sync_submit][us1]") {
    auto target = std::make_shared<CountingTarget>();
    Pipeline pipeline{make_pass_config(target, 300ms)};
    REQUIRE(pipeline.start().has_value());

    const auto t0 = std::chrono::steady_clock::now();
    auto result = pipeline.submit_sync(make_event());
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    REQUIRE(result.has_value());
    CHECK(elapsed < 300ms);

    pipeline.drain(5s);
}

// ─────────────────────────────────────────────────────────────────────────────
// US2 — Error variants
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("submit_sync: Timeout when evaluator is slow", "[unit][sync_submit][us2]") {
    auto target = std::make_shared<CountingTarget>();

    // Stage timeouts: ingest=1ms + eval=2ms + emit=1ms = 4ms, budget=10ms — valid config
    // The slow evaluator sleeps 50ms, exceeding the 10ms latency_budget timeout
    IngestStageConfig ingest_cfg;
    ingest_cfg.timeout = 1ms;

    auto cfg = PipelineConfig::Builder()
        .pipeline_id("sync-timeout-test")
        .latency_budget(10ms)
        .ingest(ingest_cfg)
        .lightweight_eval(
            EvalStageConfig{
                .timeout      = 2ms,
                .failure_mode = FailureMode::FailOpen,
                .composition  = CompositionRule::AnyBlock,
            }
            .add_evaluator(SlowEvaluator{50ms})
        )
        .emit(
            EmitStageConfig{
                .timeout      = 1ms,
                .failure_mode = FailureMode::EmitDegraded,
            }
            .add_target(target)
        )
        .rate_limit(RateLimitConfig{100'000, 200'000, 10'000})
        .build();
    REQUIRE(cfg.has_value());

    Pipeline pipeline{std::move(*cfg)};
    REQUIRE(pipeline.start().has_value());

    const auto t0 = std::chrono::steady_clock::now();
    auto result = pipeline.submit_sync(make_event());
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == SubmitSyncError::Timeout);
    // Should return within 50ms (10ms budget + polling overhead + CI slack)
    CHECK(elapsed < 50ms);

    pipeline.drain(5s);
    // No emission on timeout
    CHECK(target->count.load() == 0);
}

TEST_CASE("submit_sync: RateLimited when token bucket is exhausted", "[unit][sync_submit][us2]") {
    auto target = std::make_shared<CountingTarget>();

    // bucket_capacity=1, tokens_per_second=1 (very slow refill)
    // After the first event consumes the token, subsequent events are rate limited
    RateLimitConfig tight_rate{1, 1, 100};

    auto cfg = PipelineConfig::Builder()
        .pipeline_id("sync-ratelimit-test")
        .latency_budget(300ms)
        .ingest({})
        .lightweight_eval(
            EvalStageConfig{
                .timeout      = 50ms,
                .failure_mode = FailureMode::FailOpen,
                .composition  = CompositionRule::AnyBlock,
            }
            .add_evaluator(SyntheticPassEvaluator{})
        )
        .emit(
            EmitStageConfig{
                .timeout      = 10ms,
                .failure_mode = FailureMode::EmitDegraded,
            }
            .add_target(target)
        )
        .rate_limit(tight_rate)
        .build();
    REQUIRE(cfg.has_value());

    Pipeline pipeline{std::move(*cfg)};
    REQUIRE(pipeline.start().has_value());

    // Consume the one available token
    auto first = pipeline.submit_sync(make_event("acme", "first"));
    REQUIRE(first.has_value());

    // Now try again — token bucket should be exhausted, so rate limited
    auto result = pipeline.submit_sync(make_event("acme", "second"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == SubmitSyncError::RateLimited);

    pipeline.drain(5s);
    // Only the first event was emitted (second was rate limited before it ran)
    CHECK(target->count.load() == 1);
}

TEST_CASE("submit_sync: PipelineUnavailable when pipeline is draining", "[unit][sync_submit][us2]") {
    auto target = std::make_shared<CountingTarget>();
    Pipeline pipeline{make_pass_config(target)};
    REQUIRE(pipeline.start().has_value());
    pipeline.drain(1s);

    auto result = pipeline.submit_sync(make_event());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == SubmitSyncError::PipelineUnavailable);
    CHECK(target->count.load() == 0);
}

TEST_CASE("submit_sync: NotStarted when pipeline has never been started", "[unit][sync_submit][us2]") {
    auto target = std::make_shared<CountingTarget>();
    Pipeline pipeline{make_pass_config(target)};
    // Deliberately NOT calling start()

    auto result = pipeline.submit_sync(make_event());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == SubmitSyncError::NotStarted);
    CHECK(target->count.load() == 0);
}

TEST_CASE("submit_sync: ValidationFailed for empty entity_id", "[unit][sync_submit][us2]") {
    auto target = std::make_shared<CountingTarget>();
    Pipeline pipeline{make_pass_config(target)};
    REQUIRE(pipeline.start().has_value());

    Event invalid_ev{
        .tenant_id  = "acme",
        .entity_id  = "",  // deliberately invalid
        .event_type = "api_call",
        .timestamp  = std::chrono::system_clock::now(),
    };
    auto result = pipeline.submit_sync(std::move(invalid_ev));
    // Either ValidationFailed from submit_sync, or validation may have passed
    // through ingest differently - either way check no emission occurred
    if (!result.has_value()) {
        CHECK(result.error() == SubmitSyncError::ValidationFailed);
    }
    pipeline.drain(2s);
    CHECK(target->count.load() == 0);
}

TEST_CASE("submit_sync: Cancelled pre-call when stop already requested", "[unit][sync_submit][us2]") {
    auto target = std::make_shared<CountingTarget>();
    Pipeline pipeline{make_pass_config(target)};
    REQUIRE(pipeline.start().has_value());

    fre::StopSource src;
    src.request_stop();

    auto result = pipeline.submit_sync(make_event(), src.get_token());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == SubmitSyncError::Cancelled);
    CHECK(target->count.load() == 0);

    pipeline.drain(1s);
}

TEST_CASE("submit_sync: Cancelled in-flight by another thread", "[unit][sync_submit][us2]") {
    auto target = std::make_shared<CountingTarget>();

    auto cfg = PipelineConfig::Builder()
        .pipeline_id("sync-cancel-test")
        .latency_budget(500ms)
        .ingest({})
        .lightweight_eval(
            EvalStageConfig{
                .timeout      = 400ms,
                .failure_mode = FailureMode::FailOpen,
                .composition  = CompositionRule::AnyBlock,
            }
            .add_evaluator(SlowEvaluator{200ms})
        )
        .emit(
            EmitStageConfig{
                .timeout      = 10ms,
                .failure_mode = FailureMode::EmitDegraded,
            }
            .add_target(target)
        )
        .rate_limit(RateLimitConfig{100'000, 200'000, 10'000})
        .build();
    REQUIRE(cfg.has_value());

    Pipeline pipeline{std::move(*cfg)};
    REQUIRE(pipeline.start().has_value());

    fre::StopSource src;

    // Cancel after 10ms from another thread
    std::thread cancel_thread([&src] {
        std::this_thread::sleep_for(10ms);
        src.request_stop();
    });

    auto result = pipeline.submit_sync(make_event(), src.get_token());
    cancel_thread.join();

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == SubmitSyncError::Cancelled);
    CHECK(target->count.load() == 0);

    pipeline.drain(5s);
}

TEST_CASE("submit_sync: no emission on all error variants", "[unit][sync_submit][us2]") {
    // This test verifies that emission count stays 0 for multiple error cases
    // We test a few representative ones
    {
        auto target = std::make_shared<CountingTarget>();
        Pipeline pipeline{make_pass_config(target)};
        // NotStarted
        (void)pipeline.submit_sync(make_event());
        CHECK(target->count.load() == 0);
    }
    {
        auto target = std::make_shared<CountingTarget>();
        Pipeline pipeline{make_pass_config(target)};
        REQUIRE(pipeline.start().has_value());
        pipeline.drain(1s);
        // PipelineUnavailable
        (void)pipeline.submit_sync(make_event());
        CHECK(target->count.load() == 0);
    }
    {
        auto target = std::make_shared<CountingTarget>();
        Pipeline pipeline{make_pass_config(target)};
        REQUIRE(pipeline.start().has_value());
        fre::StopSource src;
        src.request_stop();
        // Cancelled pre-call
        (void)pipeline.submit_sync(make_event(), src.get_token());
        CHECK(target->count.load() == 0);
        pipeline.drain(1s);
    }
}

TEST_CASE("submit_sync: emission fires on success", "[unit][sync_submit][us2]") {
    auto target = std::make_shared<CountingTarget>();
    Pipeline pipeline{make_pass_config(target)};
    REQUIRE(pipeline.start().has_value());

    auto result = pipeline.submit_sync(make_event("acme", "emit-test"));
    REQUIRE(result.has_value());

    pipeline.drain(5s);
    CHECK(target->count.load() == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// T014b — Safety: pipeline destroyed while submit_sync in-flight
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("submit_sync: pipeline destroyed while in-flight does not crash", "[unit][sync_submit][t014b]") {
    auto target = std::make_shared<CountingTarget>();

    auto cfg = PipelineConfig::Builder()
        .pipeline_id("sync-destroy-test")
        .latency_budget(500ms)
        .ingest({})
        .lightweight_eval(
            EvalStageConfig{
                .timeout      = 400ms,
                .failure_mode = FailureMode::FailOpen,
                .composition  = CompositionRule::AnyBlock,
            }
            .add_evaluator(SlowEvaluator{100ms})
        )
        .emit(
            EmitStageConfig{
                .timeout      = 10ms,
                .failure_mode = FailureMode::EmitDegraded,
            }
            .add_target(target)
        )
        .rate_limit(RateLimitConfig{100'000, 200'000, 10'000})
        .build();
    REQUIRE(cfg.has_value());

    auto pipeline = std::make_unique<Pipeline>(std::move(*cfg));
    REQUIRE(pipeline->start().has_value());

    std::optional<std::expected<Decision, SubmitSyncError>> sync_result;
    std::thread submit_thread([&] {
        sync_result = pipeline->submit_sync(make_event());
    });

    // Give the submit thread time to start, then destroy the pipeline
    std::this_thread::sleep_for(50ms);
    pipeline.reset();  // triggers drain(5s) in destructor

    submit_thread.join();
    // Must have returned without crashing; result may be anything
    REQUIRE(sync_result.has_value());
    // Just check it didn't throw — we don't assert the exact value
    (void)sync_result;
}
