#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/testing/pipeline_harness.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;

// ─── Minimal pass-through evaluators for benchmarking ────────────────────────

namespace {

struct NoOpLightweightEval {
    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& /*event*/) const noexcept {
        fre::EvaluatorResult r;
        r.evaluator_id = "noop";
        r.verdict      = fre::Verdict::Pass;
        return r;
    }
};

struct NoOpInferenceEval {
    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& /*event*/) const noexcept {
        fre::EvaluatorResult r;
        r.evaluator_id = "noop_inf";
        r.verdict      = fre::Verdict::Pass;
        r.score        = 0.1f;
        return r;
    }
};

fre::PipelineConfig make_5stage_config() {
    fre::EvalStageConfig eval_cfg;
    eval_cfg.add_evaluator(NoOpLightweightEval{});

    fre::InferenceStageConfig inf_cfg;
    inf_cfg.add_evaluator(NoOpInferenceEval{});
    inf_cfg.timeout = 50ms;

    fre::EmitStageConfig emit_cfg;
    emit_cfg.add_noop_target();  // PipelineTestHarness injects its own capturing target

    // Raise rate limits well above benchmark volume so no events are dropped.
    fre::RateLimitConfig rate_cfg;
    rate_cfg.bucket_capacity   = 100'000;
    rate_cfg.tokens_per_second = 500'000;
    rate_cfg.max_concurrent    = 20'000;

    auto result = fre::PipelineConfig::Builder{}
        .pipeline_id("bench-pipeline")
        .rate_limit(rate_cfg)
        .eval_config(std::move(eval_cfg))
        .inference_config(std::move(inf_cfg))
        .emit_config(std::move(emit_cfg))
        .build();

    if (!result.has_value()) {
        throw std::runtime_error{"benchmark config build failed"};
    }
    return std::move(*result);
}

}  // namespace

// ─── Benchmark ───────────────────────────────────────────────────────────────

TEST_CASE("P99 latency benchmark — 5-stage pipeline with stub evaluators",
          "[benchmark][latency]")
{
    using namespace std::chrono_literals;

    fre::testing::PipelineTestHarness harness{make_5stage_config()};
    REQUIRE(harness.start().has_value());

    // Warm-up: submit 100 events to prime thread pools and caches.
    {
        std::vector<fre::Event> warmup_events(100);
        for (auto& ev : warmup_events) {
            ev.tenant_id  = "bench-tenant";
            ev.entity_id  = "bench-entity";
            ev.event_type = "warmup";
        }
        harness.submit_events(warmup_events);
        harness.wait_for_decisions(100, 5000ms);
        harness.clear_decisions();
    }

    BENCHMARK("submit 1000 events and collect elapsed_us") {
        constexpr std::size_t N = 1'000;
        std::vector<fre::Event> events(N);
        for (auto& ev : events) {
            ev.tenant_id  = "bench-tenant";
            ev.entity_id  = "bench-entity";
            ev.event_type = "benchmark";
        }

        harness.clear_decisions();
        harness.submit_events(events);
        auto decisions_span = harness.wait_for_decisions(N, 30'000ms);

        if (!decisions_span.has_value() || decisions_span->size() < N) {
            throw std::runtime_error{"benchmark: not all decisions collected"};
        }

        // Collect elapsed_us values and compute p99
        std::vector<uint64_t> latencies;
        latencies.reserve(decisions_span->size());
        for (const auto& d : *decisions_span) {
            latencies.push_back(d.elapsed_us);
        }
        std::sort(latencies.begin(), latencies.end());

        const std::size_t p50_idx = latencies.size() * 50 / 100;
        const std::size_t p99_idx = latencies.size() * 99 / 100;

        // Return p99 to the benchmark framework so it is visible in output
        return latencies[p99_idx];
    };

    harness.drain(5000ms);
}
