#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/testing/pipeline_harness.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ─── Stub evaluator ──────────────────────────────────────────────────────────

namespace {

struct NoOpEval {
    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& /*event*/) const noexcept {
        fre::EvaluatorResult r;
        r.evaluator_id = "noop";
        r.verdict      = fre::Verdict::Pass;
        return r;
    }
};

}  // namespace

// ─── Tests ───────────────────────────────────────────────────────────────────

TEST_CASE("Sustained load: 10 tenants × 5000 events, P99 ≤ 300ms",
          "[integration][load][constitution-vi]")
{
    constexpr int NUM_TENANTS        = 10;
    constexpr int EVENTS_PER_TENANT  = 5000;
    constexpr int TOTAL_EVENTS       = NUM_TENANTS * EVENTS_PER_TENANT;
    constexpr uint64_t P99_LIMIT_US  = 300'000;  // 300ms in microseconds

    fre::EvalStageConfig eval_cfg;
    eval_cfg.add_evaluator(NoOpEval{});

    fre::EmitStageConfig emit_cfg;
    emit_cfg.add_noop_target();  // PipelineTestHarness injects its own capturing target

    // Raise rate limits well above the load volume so no events are dropped.
    fre::RateLimitConfig rate_cfg;
    rate_cfg.bucket_capacity  = 100'000;
    rate_cfg.tokens_per_second = 200'000;
    rate_cfg.max_concurrent   = 10'000;

    auto config_result = fre::PipelineConfig::Builder{}
        .pipeline_id("load-test-pipeline")
        .rate_limit(rate_cfg)
        .eval_config(std::move(eval_cfg))
        .emit_config(std::move(emit_cfg))
        .build();
    REQUIRE(config_result.has_value());

    fre::testing::PipelineTestHarness harness{std::move(*config_result)};
    REQUIRE(harness.start().has_value());

    // Submit all events concurrently from NUM_TENANTS threads
    std::vector<std::thread> submitters;
    submitters.reserve(NUM_TENANTS);

    std::atomic<int> submit_errors{0};

    for (int tenant_idx = 0; tenant_idx < NUM_TENANTS; ++tenant_idx) {
        submitters.emplace_back([&harness, tenant_idx, &submit_errors]() {
            const std::string tenant_id = "load-tenant-" + std::to_string(tenant_idx);
            const std::string entity_id = "entity-" + std::to_string(tenant_idx);

            for (int i = 0; i < EVENTS_PER_TENANT; ++i) {
                fre::Event ev{};
                ev.tenant_id  = tenant_id;
                ev.entity_id  = entity_id;
                ev.event_type = "load_test";

                auto result = harness.submit_events(std::span<const fre::Event>{&ev, 1});
                if (!result.has_value()) {
                    ++submit_errors;
                }
            }
        });
    }

    for (auto& t : submitters) {
        t.join();
    }

    // Collect all decisions (wait up to 60 seconds for all to arrive)
    auto decisions_result = harness.wait_for_decisions(
        static_cast<std::size_t>(TOTAL_EVENTS), 60'000ms);

    REQUIRE(decisions_result.has_value());
    auto decisions = *decisions_result;

    // ─── Assertion 1: all events produced a decision ──────────────────────────
    REQUIRE(decisions.size() == static_cast<std::size_t>(TOTAL_EVENTS));

    // ─── Assertion 2: P99 latency ≤ 300ms ────────────────────────────────────
    std::vector<uint64_t> latencies;
    latencies.reserve(decisions.size());
    for (const auto& d : decisions) {
        latencies.push_back(d.elapsed_us);
    }
    std::sort(latencies.begin(), latencies.end());

    const std::size_t p99_idx  = latencies.size() * 99 / 100;
    const uint64_t    p99_us   = latencies[p99_idx];

    INFO("Overall P99 latency: " << p99_us << "µs (limit: " << P99_LIMIT_US << "µs)");
    REQUIRE(p99_us <= P99_LIMIT_US);

    // ─── Assertion 3: per-tenant P99 ≤ 300ms (no tenant blocked by another) ──
    for (int tenant_idx = 0; tenant_idx < NUM_TENANTS; ++tenant_idx) {
        const std::string expected_tenant = "load-tenant-" + std::to_string(tenant_idx);

        std::vector<uint64_t> tenant_latencies;
        tenant_latencies.reserve(EVENTS_PER_TENANT);
        for (const auto& d : decisions) {
            if (d.tenant_id == expected_tenant) {
                tenant_latencies.push_back(d.elapsed_us);
            }
        }

        if (!tenant_latencies.empty()) {
            std::sort(tenant_latencies.begin(), tenant_latencies.end());
            const std::size_t t_p99_idx = tenant_latencies.size() * 99 / 100;
            const uint64_t    t_p99_us  = tenant_latencies[t_p99_idx];

            INFO("Tenant " << expected_tenant << " P99: " << t_p99_us << "µs");
            REQUIRE(t_p99_us <= P99_LIMIT_US);
        }
    }

    harness.drain(5000ms);
}
