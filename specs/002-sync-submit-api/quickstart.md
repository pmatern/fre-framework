# Quickstart: Synchronous Blocking Submit API

**Feature**: 002-sync-submit-api
**Date**: 2026-03-15

---

## Scenario 1: Basic blocking submit (no cancellation)

```cpp
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/pipeline/sync_submit.hpp>

struct MyEvaluator {
    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& event) {
        fre::EvaluatorResult r;
        r.evaluator_id = "my_eval";
        r.verdict      = fre::Verdict::Pass;
        return r;
    }
};

int main() {
    auto cfg = fre::PipelineConfig::Builder{}
        .pipeline_id("demo")
        .eval_config(fre::EvalStageConfig{}.add_evaluator(MyEvaluator{}))
        .emit_config(fre::EmitStageConfig{}.add_stdout_target())
        .build();

    fre::Pipeline pipeline{std::move(*cfg)};
    pipeline.start();

    fre::Event ev;
    ev.tenant_id  = "acme";
    ev.entity_id  = "user-1";
    ev.event_type = "login";

    // Block until the pipeline decides — no callback, no emission target wiring needed.
    auto result = pipeline.submit_sync(ev);

    if (result) {
        // Decision is in *result
        // stdout target has already fired (emission targets run before submit_sync returns)
        assert(result->final_verdict == fre::Verdict::Pass);
    } else {
        // Handle error
        switch (result.error()) {
        case fre::SubmitSyncError::Timeout:          // decision not produced in time
        case fre::SubmitSyncError::RateLimited:       // too many concurrent submits
        case fre::SubmitSyncError::PipelineUnavailable:
        case fre::SubmitSyncError::NotStarted:
        case fre::SubmitSyncError::ValidationFailed:
        case fre::SubmitSyncError::Cancelled:
            break;
        }
    }

    pipeline.drain(std::chrono::milliseconds{500});
}
```

---

## Scenario 2: Blocking submit with caller-initiated cancellation

```cpp
#include <stop_token>

// Create a stop source the caller controls.
std::stop_source source;

// Optionally: cancel from another thread after 50 ms
std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    source.request_stop();
});

fre::Event ev;
ev.tenant_id  = "acme";
ev.entity_id  = "user-2";
ev.event_type = "api_call";

auto result = pipeline.submit_sync(ev, source.get_token());

canceller.join();

if (!result && result.error() == fre::SubmitSyncError::Cancelled) {
    // Caller cancelled before decision arrived — no decision was emitted to targets
}
```

---

## Scenario 3: Mixing async and blocking on the same pipeline

```cpp
// Async submit (fire-and-forget) — decision goes to emission targets only
pipeline.submit(ev_async);

// Blocking submit — caller receives the Decision AND emission targets fire
auto decision = pipeline.submit_sync(ev_sync);

// Both share the same rate-limit pool and pipeline state — no interference.
```

---

## Scenario 4: Integration test using submit_sync (no PipelineTestHarness needed)

```cpp
#include <catch2/catch_test_macros.hpp>

TEST_CASE("evaluator sets Flag verdict") {
    auto cfg = fre::PipelineConfig::Builder{}
        .pipeline_id("test")
        .eval_config(fre::EvalStageConfig{}.add_evaluator(FlagAllEvaluator{}))
        .emit_config(fre::EmitStageConfig{}.add_noop_target())
        .build();

    fre::Pipeline pipeline{std::move(*cfg)};
    pipeline.start();

    fre::Event ev;
    ev.tenant_id  = "test-tenant";
    ev.entity_id  = "entity-1";
    ev.event_type = "check";

    auto result = pipeline.submit_sync(ev);

    REQUIRE(result.has_value());
    CHECK(result->final_verdict == fre::Verdict::Flag);

    pipeline.drain(std::chrono::milliseconds{500});
}
```

---

## Error handling reference

| Error variant | Cause | Typical response |
|---|---|---|
| `Timeout` | Pipeline stages took longer than latency budget | Retry with backoff; check evaluator performance |
| `RateLimited` | `max_concurrent` or token bucket exceeded | Back off and retry; raise rate-limit config for load tests |
| `PipelineUnavailable` | Pipeline draining or stopped | Wait for drain to complete; re-create pipeline if needed |
| `NotStarted` | `pipeline.start()` never called | Call `pipeline.start()` before submitting |
| `ValidationFailed` | Missing `tenant_id`, `entity_id`, or `event_type`; clock skew | Fix the event fields |
| `Cancelled` | Caller called `stop_source.request_stop()` | Check cancellation logic; retry if appropriate |
