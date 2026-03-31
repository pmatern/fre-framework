#ifdef FRE_ENABLE_DUCKDB

#include <fre/evaluator/allow_deny_evaluator.hpp>
#include <fre/evaluator/threshold_evaluator.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/state/duckdb_window_store.hpp>
#include <fre/state/window_store.hpp>
#include <fre/state/write_back_window_store.hpp>
#include <fre/testing/pipeline_harness.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

// RAII guard: removes all registered paths in its destructor, whether the test
// passes or fails.
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
make_bench_config(std::shared_ptr<fre::WriteBackWindowStore> store,
                  const std::filesystem::path&               deny_list_path)
{
    fre::EvalStageConfig eval_cfg;
    eval_cfg.composition  = fre::CompositionRule::AnyBlock;
    eval_cfg.timeout      = 50ms;
    eval_cfg.failure_mode = fre::FailureMode::FailOpen;
    eval_cfg.add_evaluator(fre::AllowDenyEvaluator{fre::AllowDenyEvaluatorConfig{
        .deny_list_path = deny_list_path,
        .match_field    = fre::AllowDenyMatchField::EntityId,
        .default_verdict = fre::Verdict::Pass,
    }});
    eval_cfg.add_evaluator(fre::ThresholdEvaluator<fre::WriteBackWindowStore>{
        fre::ThresholdEvaluatorConfig{
            .window_duration = 60s,
            .aggregation     = fre::AggregationFn::Count,
            .group_by        = fre::GroupBy::EntityId,
            .threshold       = 200.0,
            .window_name     = "bench_threshold",
        },
        store,
    });

    fre::InferenceStageConfig inf_cfg;
    inf_cfg.timeout = 50ms;
    inf_cfg.add_evaluator(NoOpInferenceEval{});

    fre::RateLimitConfig rate_cfg;
    rate_cfg.bucket_capacity   = 100'000;
    rate_cfg.tokens_per_second = 500'000;
    rate_cfg.max_concurrent    = 20'000;

    fre::EmitStageConfig emit_cfg;
    emit_cfg.add_noop_target();

    return fre::PipelineConfig::Builder{}
        .pipeline_id("bench-realistic")
        .rate_limit(rate_cfg)
        .eval_config(std::move(eval_cfg))
        .inference_config(std::move(inf_cfg))
        .emit_config(std::move(emit_cfg))
        .build();
}

}  // namespace

// ─── Benchmark ───────────────────────────────────────────────────────────────

TEST_CASE("P99 latency benchmark — real evaluators + DuckDB", "[benchmark][realistic]")
{
    const std::string pid_str = std::to_string(static_cast<long>(getpid()));

    TempFileGuard guard;
    guard.paths.push_back(std::filesystem::temp_directory_path() /
                          ("fre_bench_" + pid_str + ".duckdb"));
    guard.paths.push_back(std::filesystem::temp_directory_path() /
                          ("fre_bench_deny_" + pid_str + ".txt"));

    write_file(guard.paths[1], "blocked-entity\n");

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
        primary, warm, fre::WriteBackConfig{.flush_interval_ms = 200});

    auto cfg = make_bench_config(store, guard.paths[1]);
    REQUIRE(cfg.has_value());

    fre::testing::PipelineTestHarness harness{std::move(*cfg)};
    REQUIRE(harness.start().has_value());

    // ─── Warm-up ─────────────────────────────────────────────────────────────
    {
        std::vector<fre::Event> warmup(100);
        for (auto& ev : warmup) {
            ev.tenant_id  = "bench-tenant";
            ev.entity_id  = "warmup-entity";
            ev.event_type = "warmup";
        }
        harness.submit_events(warmup);
        harness.wait_for_decisions(100, 5000ms);
        harness.clear_decisions();
    }

    // ─── Benchmark ───────────────────────────────────────────────────────────
    BENCHMARK("realistic pipeline: AllowDeny + Threshold + DuckDB") {
        constexpr std::size_t N = 1'000;

        // 500 high-volume (crosses threshold=200 at event 201),
        // 100 blocked (deny list), 400 normal (always Pass)
        std::vector<fre::Event> events;
        events.reserve(N);
        const auto now = std::chrono::system_clock::now();

        for (int i = 0; i < 500; ++i) {
            fre::Event ev{};
            ev.tenant_id  = "bench-tenant";
            ev.entity_id  = "high-volume-entity";
            ev.event_type = "api_call";
            ev.timestamp  = now;
            events.push_back(ev);
        }
        for (int i = 0; i < 100; ++i) {
            fre::Event ev{};
            ev.tenant_id  = "bench-tenant";
            ev.entity_id  = "blocked-entity";
            ev.event_type = "api_call";
            ev.timestamp  = now;
            events.push_back(ev);
        }
        for (int i = 0; i < 400; ++i) {
            fre::Event ev{};
            ev.tenant_id  = "bench-tenant";
            ev.entity_id  = "normal-entity";
            ev.event_type = "api_call";
            ev.timestamp  = now;
            events.push_back(ev);
        }

        harness.clear_decisions();
        harness.submit_events(events);
        auto span = harness.wait_for_decisions(N, 30'000ms);

        if (!span.has_value() || span->size() < N) {
            throw std::runtime_error{"benchmark: not all decisions collected"};
        }

        std::vector<uint64_t> latencies;
        latencies.reserve(span->size());
        for (const auto& d : *span) {
            latencies.push_back(d.elapsed_us);
        }
        std::sort(latencies.begin(), latencies.end());

        const std::size_t p99_idx = latencies.size() * 99 / 100;
        return latencies[p99_idx];
    };

    // ─── Post-benchmark correctness assertions ────────────────────────────────
    harness.drain(5000ms);

    const auto decisions = harness.decisions();
    REQUIRE(std::any_of(decisions.begin(), decisions.end(),
        [](const fre::Decision& d) { return d.final_verdict == fre::Verdict::Flag; }));
    REQUIRE(std::any_of(decisions.begin(), decisions.end(),
        [](const fre::Decision& d) { return d.final_verdict == fre::Verdict::Block; }));
}

#endif  // FRE_ENABLE_DUCKDB
