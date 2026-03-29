#ifdef FRE_ENABLE_DUCKDB

/// Integration tests: full pipeline using ThresholdEvaluator<WriteBackWindowStore>.
///
/// Tests:
///   1. Functional parity with InProcessWindowStore baseline (threshold flagging).
///   2. State survives WriteBackWindowStore destruction + recreation (restart recovery).
///   3. Pipeline operates correctly when DuckDB is unavailable (hot path unaffected).

#include <fre/core/concepts.hpp>
#include <fre/evaluator/threshold_evaluator.hpp>
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/state/duckdb_window_store.hpp>
#include <fre/state/window_store.hpp>
#include <fre/state/write_back_window_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

using namespace fre;
using namespace std::chrono_literals;

// ─── Helpers ─────────────────────────────────────────────────────────────────

struct VerdictCollector {
    mutable std::mutex    mu;
    std::vector<Decision> decisions;
    std::atomic<int>      count{0};

    std::expected<void, EmissionError> emit(Decision d) {
        std::lock_guard lock{mu};
        decisions.push_back(std::move(d));
        ++count;
        return {};
    }
};
static_assert(EmissionTarget<VerdictCollector>);

static std::shared_ptr<DuckDbWindowStore> make_warm() {
    return std::make_shared<DuckDbWindowStore>(DuckDbConfig{
        .db_path              = "",
        .parquet_archive_dir  = "",
        .flush_interval_ms    = 0,
        .window_ms            = 60000,
        .warm_epoch_retention = 3,
    });
}

// Build a pipeline using ThresholdEvaluator<WriteBackWindowStore>.
static auto make_pipeline(std::shared_ptr<WriteBackWindowStore> store,
                           std::shared_ptr<VerdictCollector>     target,
                           double threshold = 100.0)
{
    RateLimitConfig rate_cfg;
    rate_cfg.bucket_capacity   = 10'000;
    rate_cfg.tokens_per_second = 50'000;
    rate_cfg.max_concurrent    = 1'000;

    return PipelineConfig::Builder()
        .pipeline_id("write-back-test")
        .latency_budget(300ms)
        .rate_limit(rate_cfg)
        .ingest({})
        .lightweight_eval(
            EvalStageConfig{
                .timeout      = 50ms,
                .failure_mode = FailureMode::FailOpen,
                .composition  = CompositionRule::AnyBlock,
            }
            .add_evaluator(ThresholdEvaluator<WriteBackWindowStore>{
                ThresholdEvaluatorConfig{
                    .window_duration = 60s,
                    .aggregation     = AggregationFn::Count,
                    .group_by        = GroupBy::EntityId,
                    .threshold       = threshold,
                },
                store
            })
        )
        .emit(EmitStageConfig{.timeout = 10ms}.add_target(target))
        .build();
}

// ─── Functional parity with InProcessWindowStore ─────────────────────────────

TEST_CASE("WriteBack pipeline: same flagging behaviour as InProcessWindowStore baseline",
          "[write_back][integration]")
{
    auto warm    = make_warm();
    auto primary = std::make_shared<InProcessWindowStore>();
    auto store   = std::make_shared<WriteBackWindowStore>(
        primary, warm, WriteBackConfig{.flush_interval_ms = 0});
    auto target  = std::make_shared<VerdictCollector>();

    auto cfg = make_pipeline(store, target, 100.0);
    REQUIRE(cfg.has_value());
    Pipeline pipeline{std::move(*cfg)};
    REQUIRE(pipeline.start().has_value());

    const auto now = std::chrono::system_clock::now();

    GIVEN("150 events for entity A (crosses threshold at 101)") {
        for (int i = 0; i < 150; ++i) {
            REQUIRE(pipeline.submit(Event{
                .tenant_id  = "acme",
                .entity_id  = "entity-A",
                .event_type = "api_call",
                .timestamp  = now,
            }).has_value());
        }

        GIVEN("50 events for entity B (stays under threshold)") {
            for (int i = 0; i < 50; ++i) {
                REQUIRE(pipeline.submit(Event{
                    .tenant_id  = "acme",
                    .entity_id  = "entity-B",
                    .event_type = "api_call",
                    .timestamp  = now,
                }).has_value());
            }

            pipeline.drain(10s);
            REQUIRE(target->count.load() == 200);

            std::lock_guard lock{target->mu};

            THEN("entity A events 101-150 are flagged") {
                int a_flagged = 0, a_total = 0;
                for (const auto& d : target->decisions) {
                    if (d.entity_id == "entity-A") {
                        ++a_total;
                        if (d.final_verdict == Verdict::Flag) ++a_flagged;
                    }
                }
                REQUIRE(a_total == 150);
                REQUIRE(a_flagged >= 50);
            }

            THEN("all entity B events pass") {
                for (const auto& d : target->decisions) {
                    if (d.entity_id == "entity-B") {
                        REQUIRE(d.final_verdict == Verdict::Pass);
                    }
                }
            }
        }
    }
}

// ─── Restart recovery ────────────────────────────────────────────────────────

TEST_CASE("WriteBack pipeline: counts survive WriteBackWindowStore recreation",
          "[write_back][integration]")
{
    // Shared warm tier persists across restarts.
    auto warm = make_warm();

    const auto now = std::chrono::system_clock::now();

    GIVEN("first pipeline run: submit 80 events (below threshold of 100)") {
        {
            auto primary = std::make_shared<InProcessWindowStore>();
            auto store = std::make_shared<WriteBackWindowStore>(
                primary, warm, WriteBackConfig{.flush_interval_ms = 0});
            auto target = std::make_shared<VerdictCollector>();

            auto cfg = make_pipeline(store, target, 100.0);
            REQUIRE(cfg.has_value());
            Pipeline pipeline{std::move(*cfg)};
            REQUIRE(pipeline.start().has_value());

            for (int i = 0; i < 80; ++i) {
                REQUIRE(pipeline.submit(Event{
                    .tenant_id  = "acme",
                    .entity_id  = "user-restart",
                    .event_type = "api_call",
                    .timestamp  = now,
                }).has_value());
            }
            pipeline.drain(10s);

            // Destructor flushes dirty entries to warm tier before destroying.
        }

        THEN("second pipeline run recovers count; next 30 events trigger flagging") {
            auto primary2 = std::make_shared<InProcessWindowStore>();
            auto store2   = std::make_shared<WriteBackWindowStore>(
                primary2, warm, WriteBackConfig{.flush_interval_ms = 0});
            auto target2  = std::make_shared<VerdictCollector>();

            auto cfg2 = make_pipeline(store2, target2, 100.0);
            REQUIRE(cfg2.has_value());
            Pipeline pipeline2{std::move(*cfg2)};
            REQUIRE(pipeline2.start().has_value());

            // Submit 30 more events — count resumes from ~80, threshold is 100.
            for (int i = 0; i < 30; ++i) {
                REQUIRE(pipeline2.submit(Event{
                    .tenant_id  = "acme",
                    .entity_id  = "user-restart",
                    .event_type = "api_call",
                    .timestamp  = now,
                }).has_value());
            }
            pipeline2.drain(10s);

            std::lock_guard lock{target2->mu};
            int flagged = 0;
            for (const auto& d : target2->decisions) {
                if (d.final_verdict == Verdict::Flag) ++flagged;
            }
            // Events 101-110 (10 events) should be flagged (count resumes from 80).
            REQUIRE(flagged >= 1);
        }
    }
}

// ─── DuckDB unavailable — hot path unaffected ────────────────────────────────

TEST_CASE("WriteBack pipeline: pipeline operates when DuckDB is unavailable",
          "[write_back][integration]")
{
    auto warm_unavail = std::make_shared<DuckDbWindowStore>(DuckDbConfig{
        .db_path = "/no/such/path/write_back_test.duckdb",
    });
    REQUIRE_FALSE(warm_unavail->is_available());

    auto primary = std::make_shared<InProcessWindowStore>();
    auto store   = std::make_shared<WriteBackWindowStore>(
        primary, warm_unavail, WriteBackConfig{.flush_interval_ms = 0});
    auto target  = std::make_shared<VerdictCollector>();

    REQUIRE(store->is_available());

    auto cfg = make_pipeline(store, target, 50.0);
    REQUIRE(cfg.has_value());
    Pipeline pipeline{std::move(*cfg)};
    REQUIRE(pipeline.start().has_value());

    const auto now = std::chrono::system_clock::now();

    for (int i = 0; i < 100; ++i) {
        REQUIRE(pipeline.submit(Event{
            .tenant_id  = "acme",
            .entity_id  = "user-noduckdb",
            .event_type = "api_call",
            .timestamp  = now,
        }).has_value());
    }
    pipeline.drain(10s);

    // Pipeline processed all events; events 51-100 should be flagged.
    REQUIRE(target->count.load() == 100);
    int flagged = 0;
    {
        std::lock_guard lock{target->mu};
        for (const auto& d : target->decisions) {
            if (d.final_verdict == Verdict::Flag) ++flagged;
        }
    }
    REQUIRE(flagged >= 50);
}

#endif  // FRE_ENABLE_DUCKDB
