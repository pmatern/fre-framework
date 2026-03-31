#ifdef FRE_ENABLE_DUCKDB

#include <fre/evaluator/allow_deny_evaluator.hpp>
#include <fre/evaluator/threshold_evaluator.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/state/duckdb_window_store.hpp>
#include <fre/state/window_store.hpp>
#include <fre/state/write_back_window_store.hpp>
#include <fre/testing/pipeline_harness.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

struct TempFileGuard {
    std::vector<std::filesystem::path> paths;

    ~TempFileGuard() {
        for (const auto& p : paths) {
            std::filesystem::remove(p);
        }
    }
};

static void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream ofs{path};
    if (!ofs) {
        throw std::runtime_error{"TempFileGuard: cannot write " + path.string()};
    }
    ofs << content;
}

// ─── Stub inference evaluator ─────────────────────────────────────────────────

struct NoOpInferenceEval {
    [[nodiscard]] std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& /*event*/) const noexcept {
        fre::EvaluatorResult r;
        r.evaluator_id = "noop_inf";
        r.verdict      = fre::Verdict::Pass;
        r.score        = 0.0f;
        return r;
    }
};

// ─── Pipeline config builder ─────────────────────────────────────────────────

static std::expected<fre::PipelineConfig, fre::Error>
make_load_config(std::shared_ptr<fre::WriteBackWindowStore> store,
                 const std::filesystem::path&               deny_list_path)
{
    fre::EvalStageConfig eval_cfg;
    eval_cfg.composition  = fre::CompositionRule::AnyBlock;
    eval_cfg.timeout      = 50ms;
    eval_cfg.failure_mode = fre::FailureMode::FailOpen;
    eval_cfg.add_evaluator(fre::AllowDenyEvaluator{fre::AllowDenyEvaluatorConfig{
        .deny_list_path  = deny_list_path,
        .match_field     = fre::AllowDenyMatchField::EntityId,
        .default_verdict = fre::Verdict::Pass,
    }});
    eval_cfg.add_evaluator(fre::ThresholdEvaluator<fre::WriteBackWindowStore>{
        fre::ThresholdEvaluatorConfig{
            .window_duration = 60s,
            .aggregation     = fre::AggregationFn::Count,
            .group_by        = fre::GroupBy::EntityId,
            .threshold       = 200.0,
            .window_name     = "load_threshold",
        },
        store,
    });

    fre::InferenceStageConfig inf_cfg;
    inf_cfg.timeout = 50ms;
    inf_cfg.add_evaluator(NoOpInferenceEval{});

    fre::RateLimitConfig rate_cfg;
    rate_cfg.bucket_capacity   = 100'000;
    rate_cfg.tokens_per_second = 200'000;
    rate_cfg.max_concurrent    = 10'000;

    fre::EmitStageConfig emit_cfg;
    emit_cfg.add_noop_target();

    return fre::PipelineConfig::Builder{}
        .pipeline_id("load-realistic")
        .rate_limit(rate_cfg)
        .eval_config(std::move(eval_cfg))
        .inference_config(std::move(inf_cfg))
        .emit_config(std::move(emit_cfg))
        .build();
}

}  // namespace

// ─── Load test ───────────────────────────────────────────────────────────────

TEST_CASE("Realistic load: 10 tenants x 3000 events, P99 <= 500ms",
          "[integration][load][realistic]")
{
    constexpr int NUM_TENANTS       = 10;
    constexpr int EVENTS_PER_TENANT = 3000;
    constexpr int TOTAL_EVENTS      = NUM_TENANTS * EVENTS_PER_TENANT;
    constexpr uint64_t P99_LIMIT_US = 500'000;  // 500ms

    const std::string pid_str = std::to_string(static_cast<long>(getpid()));

    TempFileGuard guard;
    guard.paths.push_back(std::filesystem::temp_directory_path() /
                          ("fre_load_" + pid_str + ".duckdb"));
    guard.paths.push_back(std::filesystem::temp_directory_path() /
                          ("fre_load_deny_" + pid_str + ".txt"));

    // One blocked entity per tenant: blocked-0 … blocked-9
    std::string deny_content;
    for (int n = 0; n < NUM_TENANTS; ++n) {
        deny_content += "blocked-" + std::to_string(n) + "\n";
    }
    write_file(guard.paths[1], deny_content);

    auto warm = std::make_shared<fre::DuckDbWindowStore>(fre::DuckDbConfig{
        .db_path              = guard.paths[0].string(),
        .parquet_archive_dir  = "",
        .flush_interval_ms    = 0,
        .window_ms            = 60000,
        .warm_epoch_retention = 3,
    });
    REQUIRE(warm->is_available());

    auto primary = std::make_shared<fre::InProcessWindowStore>();
    auto store   = std::make_shared<fre::WriteBackWindowStore>(
        primary, warm, fre::WriteBackConfig{.flush_interval_ms = 500});

    auto cfg = make_load_config(store, guard.paths[1]);
    REQUIRE(cfg.has_value());

    fre::testing::PipelineTestHarness harness{std::move(*cfg)};
    REQUIRE(harness.start().has_value());

    // ─── Warm-up ─────────────────────────────────────────────────────────────
    {
        std::vector<fre::Event> warmup(100);
        for (auto& ev : warmup) {
            ev.tenant_id  = "warmup-tenant";
            ev.entity_id  = "warmup";
            ev.event_type = "warmup";
        }
        harness.submit_events(warmup);
        harness.wait_for_decisions(100, 5000ms);
        harness.clear_decisions();
    }

    // ─── Concurrent submission: 10 threads × 3000 events ─────────────────────
    std::atomic<int> submit_errors{0};
    std::vector<std::thread> submitters;
    submitters.reserve(NUM_TENANTS);

    for (int n = 0; n < NUM_TENANTS; ++n) {
        submitters.emplace_back([&harness, &submit_errors, n]() {
            const std::string tenant_id = "load-tenant-" + std::to_string(n);
            const std::string hi_entity = "entity-"  + std::to_string(n);
            const std::string bl_entity = "blocked-" + std::to_string(n);
            const auto now = std::chrono::system_clock::now();

            // 2500 high-volume events (crosses threshold=200)
            for (int i = 0; i < 2500; ++i) {
                fre::Event ev{};
                ev.tenant_id  = tenant_id;
                ev.entity_id  = hi_entity;
                ev.event_type = "load_test";
                ev.timestamp  = now;
                auto r = harness.submit_events(std::span<const fre::Event>{&ev, 1});
                if (!r.has_value()) { ++submit_errors; }
            }
            // 500 blocked events (on deny list)
            for (int i = 0; i < 500; ++i) {
                fre::Event ev{};
                ev.tenant_id  = tenant_id;
                ev.entity_id  = bl_entity;
                ev.event_type = "load_test";
                ev.timestamp  = now;
                auto r = harness.submit_events(std::span<const fre::Event>{&ev, 1});
                if (!r.has_value()) { ++submit_errors; }
            }
        });
    }

    for (auto& t : submitters) {
        t.join();
    }

    // ─── Collect all decisions ────────────────────────────────────────────────
    auto result = harness.wait_for_decisions(
        static_cast<std::size_t>(TOTAL_EVENTS), 60'000ms);

    REQUIRE(result.has_value());
    REQUIRE(result->size() == static_cast<std::size_t>(TOTAL_EVENTS));  // SC-006

    // ─── Overall P99 ≤ 500ms ─────────────────────────────────────────────────
    std::vector<uint64_t> all_latencies;
    all_latencies.reserve(result->size());
    for (const auto& d : *result) {
        all_latencies.push_back(d.elapsed_us);
    }
    std::sort(all_latencies.begin(), all_latencies.end());

    const std::size_t p99_idx = all_latencies.size() * 99 / 100;
    const uint64_t    p99_us  = all_latencies[p99_idx];

    INFO("Overall P99 latency: " << p99_us << "µs (limit: " << P99_LIMIT_US << "µs)");
    REQUIRE(p99_us <= P99_LIMIT_US);

    // ─── Per-tenant P99 + verdict assertions ─────────────────────────────────
    const auto decisions = *result;

    for (int n = 0; n < NUM_TENANTS; ++n) {
        const std::string tenant_id = "load-tenant-" + std::to_string(n);
        const std::string hi_entity = "entity-"  + std::to_string(n);
        const std::string bl_entity = "blocked-" + std::to_string(n);

        std::vector<uint64_t> tenant_lat;
        tenant_lat.reserve(EVENTS_PER_TENANT);
        bool has_flag  = false;
        bool has_block = false;

        for (const auto& d : decisions) {
            if (d.tenant_id != tenant_id) continue;
            tenant_lat.push_back(d.elapsed_us);
            if (d.final_verdict == fre::Verdict::Flag)  has_flag  = true;
            if (d.final_verdict == fre::Verdict::Block) has_block = true;
        }

        if (!tenant_lat.empty()) {
            std::sort(tenant_lat.begin(), tenant_lat.end());
            const std::size_t t_p99_idx = tenant_lat.size() * 99 / 100;
            const uint64_t    t_p99_us  = tenant_lat[t_p99_idx];
            INFO("Tenant " << tenant_id << " P99: " << t_p99_us << "µs");
            REQUIRE(t_p99_us <= P99_LIMIT_US);
        }

        INFO("Tenant " << tenant_id << " has_flag=" << has_flag
             << " has_block=" << has_block);
        REQUIRE(has_flag);   // entity-<n> crossed threshold=200 (SC-005)
        REQUIRE(has_block);  // blocked-<n> on deny list (SC-005)
    }

    harness.drain(5000ms);
}

#endif  // FRE_ENABLE_DUCKDB
