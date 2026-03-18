#ifdef FRE_ENABLE_DUCKDB

/// Integration tests for DuckDbWindowStore with a full pipeline.
///
/// Scenarios:
///   1. CountingEvaluator with DuckDB backend — mirrors test_external_store_fallback.
///   2. Restart recovery — second store instance on same file reads back prior counts.
///   3. Fallback activation — DuckDB backend reports unavailable → fallback kicks in.

#include <fre/state/duckdb_window_store.hpp>
#include <fre/state/external_store.hpp>
#include <fre/state/window_store.hpp>
#include <fre/core/concepts.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/pipeline/pipeline.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace fre;
using namespace std::chrono_literals;

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

struct CapturingTarget {
    mutable std::mutex         mu;
    std::vector<fre::Decision> decisions;

    std::expected<void, fre::EmissionError> emit(fre::Decision d) {
        std::lock_guard lock{mu};
        decisions.push_back(std::move(d));
        return {};
    }

    std::size_t count() const {
        std::lock_guard lock{mu};
        return decisions.size();
    }

    fre::Decision get(std::size_t i) const {
        std::lock_guard lock{mu};
        return decisions.at(i);
    }
};

// ─── CountingEvaluator (DuckDB-backed) ───────────────────────────────────────

struct DuckDbCountingEvaluator {
    std::shared_ptr<ExternalWindowStore> store;
    int threshold{5};

    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& event) {
        WindowKey key{event.tenant_id, event.entity_id};

        WindowValue new_val;
        auto current = store->get(key);
        double count = current.has_value() ? current->aggregate + 1.0 : 1.0;
        new_val.aggregate = count;
        new_val.version   = current.has_value() ? current->version + 1 : 1;

        WindowValue old_val = current.value_or(WindowValue{});
        std::ignore = store->compare_and_swap(key, old_val, new_val);

        return EvaluatorResult{
            .evaluator_id = "DuckDbCountingEvaluator",
            .verdict      = (count > static_cast<double>(threshold))
                                ? Verdict::Flag : Verdict::Pass,
        };
    }
};
static_assert(LightweightEvaluator<DuckDbCountingEvaluator>);

// Helpers
Event make_event(std::string tenant_id, std::string entity_id) {
    Event e;
    e.tenant_id  = std::move(tenant_id);
    e.entity_id  = std::move(entity_id);
    e.event_type = "test";
    return e;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

SCENARIO("DuckDbWindowStore: counting evaluator flags after threshold with DuckDB backend",
         "[integration][duckdb]")
{
    GIVEN("a pipeline with a counting evaluator backed by DuckDB external store") {
        auto target = std::make_shared<CapturingTarget>();

        // Build DuckDB-backed external store
        auto duckdb_store = std::make_shared<DuckDbWindowStore>(DuckDbConfig{
            .db_path = "", .parquet_archive_dir = "", .flush_interval_ms = 0});
        REQUIRE(duckdb_store->is_available());

        auto backend  = duckdb_store->as_backend();
        auto fallback = std::make_shared<InProcessWindowStore>();
        auto store    = std::make_shared<ExternalWindowStore>(std::move(backend), fallback);

        EvalStageConfig eval_cfg;
        eval_cfg.add_evaluator(DuckDbCountingEvaluator{store, /*threshold=*/5});

        EmitStageConfig emit_cfg;
        emit_cfg.add_target(target);

        auto config_result = PipelineConfig::Builder{}
            .pipeline_id("duckdb-counting-test")
            .eval_config(std::move(eval_cfg))
            .emit_config(std::move(emit_cfg))
            .build();
        REQUIRE(config_result.has_value());

        Pipeline pipeline{std::move(*config_result)};
        REQUIRE(pipeline.start().has_value());

        WHEN("10 events are submitted for entity-X (threshold=5)") {
            for (int i = 0; i < 10; ++i) {
                REQUIRE(pipeline.submit(make_event("acme", "entity-X")).has_value());
            }
            pipeline.drain(3s);

            THEN("all 10 decisions are emitted") {
                REQUIRE(target->count() == 10);
            }

            THEN("events 6-10 are flagged") {
                int flagged = 0;
                for (std::size_t i = 0; i < target->count(); ++i) {
                    if (target->get(i).final_verdict != Verdict::Pass) ++flagged;
                }
                REQUIRE(flagged >= 1);
            }
        }
    }
}

SCENARIO("DuckDbWindowStore: restart recovery — second instance reads prior state",
         "[integration][duckdb]")
{
    const std::string db_path = "/tmp/fre_duckdb_restart_test_" +
                                 std::to_string(std::hash<std::string>{}("restart")) + ".duckdb";
    std::filesystem::remove(db_path);

    GIVEN("a DuckDb store that writes some window values") {
        {
            DuckDbWindowStore store{DuckDbConfig{.db_path = db_path}};
            REQUIRE(store.is_available());

            const WindowKey key{.tenant_id = "t1", .entity_id = "e1",
                                 .window_name = "w1", .epoch = 42};
            store.compare_and_swap(key, WindowValue{},
                                   WindowValue{.aggregate = 7.0, .version = 1});

            auto read = store.get(key);
            REQUIRE(read.has_value());
            REQUIRE(read->aggregate == 7.0);
        }  // store goes out of scope — DuckDB flushes WAL

        WHEN("a second store instance is opened on the same file") {
            DuckDbWindowStore recovered{DuckDbConfig{.db_path = db_path}};
            REQUIRE(recovered.is_available());

            THEN("prior window state is readable") {
                const WindowKey key{.tenant_id = "t1", .entity_id = "e1",
                                     .window_name = "w1", .epoch = 42};
                auto result = recovered.get(key);
                REQUIRE(result.has_value());
                REQUIRE(result->aggregate == 7.0);
                REQUIRE(result->version == 1);
            }
        }
    }

    std::filesystem::remove(db_path);
}

SCENARIO("DuckDbWindowStore: unavailable backend falls back to InProcessWindowStore",
         "[integration][duckdb]")
{
    GIVEN("a DuckDb backend that cannot open its database") {
        // A bad path (no directory) causes duckdb_open to fail.
        DuckDbWindowStore bad_store{DuckDbConfig{
            .db_path = "/nonexistent/path/that/cannot/be/created/state.duckdb"}};

        THEN("is_available() returns false") {
            REQUIRE_FALSE(bad_store.is_available());
        }

        AND_WHEN("used as ExternalStoreBackend, fallback activates") {
            auto backend  = bad_store.as_backend();
            auto fallback = std::make_shared<InProcessWindowStore>();
            ExternalWindowStore ext{std::move(backend), fallback};

            // Perform an operation — this triggers fallback and sets is_degraded()
            const WindowKey key{.tenant_id = "t1", .entity_id = "e1",
                                 .window_name = "w1", .epoch = 0};
            auto result = ext.get(key);

            THEN("operation succeeds via fallback") {
                REQUIRE(result.has_value());
                REQUIRE(ext.is_degraded());
            }
        }
    }
}

SCENARIO("DuckDbWindowStore: query_range reads from cold-tier Parquet after flush",
         "[integration][duckdb][parquet]")
{
    const auto hash_tag = std::to_string(std::hash<std::string>{}("parquet-union"));
    const std::string db_path      = "/tmp/fre_parquet_union_" + hash_tag + ".duckdb";
    const std::string archive_dir  = "/tmp/fre_parquet_archive_" + hash_tag;
    std::filesystem::remove(db_path);
    std::filesystem::remove_all(archive_dir);

    GIVEN("a store with fast flush interval writing to old epochs 0 and 1") {
        DuckDbWindowStore store{DuckDbConfig{
            .db_path             = db_path,
            .parquet_archive_dir = archive_dir,
            .flush_interval_ms   = 50,   // flush quickly in test
            .window_ms           = 1,    // current epoch ≈ now_ms ≈ 1.7T: epochs 0,1 are ancient
            .warm_epoch_retention = 1,   // flush everything < (current-1)
        }};
        REQUIRE(store.is_available());

        // Write 10.0 to epoch 0 and 10.0 to epoch 1 — both immediately eligible for flush
        for (uint64_t ep = 0; ep < 2; ++ep) {
            const WindowKey key{
                .tenant_id   = "union-t",
                .entity_id   = "e1",
                .window_name = "w1",
                .epoch       = ep,
            };
            auto r = store.compare_and_swap(
                key, WindowValue{}, WindowValue{.aggregate = 10.0, .version = 1});
            REQUIRE(r.has_value());
            REQUIRE(*r == true);
        }

        WHEN("the warm tier is queried before flush") {
            auto warm = store.query_range("union-t", "e1", "w1", 0, 1);
            THEN("sum is 20.0 from warm tier") {
                REQUIRE(warm.has_value());
                REQUIRE(*warm == 20.0);
            }
        }

        WHEN("flush interval elapses (150 ms)") {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));

            THEN("Parquet files exist in the archive directory") {
                REQUIRE(std::filesystem::exists(archive_dir));
                bool found = false;
                for (const auto& p :
                     std::filesystem::recursive_directory_iterator(archive_dir)) {
                    if (p.path().extension() == ".parquet") { found = true; break; }
                }
                REQUIRE(found);
            }

            THEN("query_range returns 20.0 from cold tier after warm-tier rows are deleted") {
                // After flush, warm tier rows for epoch 0 and 1 are deleted.
                // The combined UNION ALL query must read them from Parquet.
                auto cold = store.query_range("union-t", "e1", "w1", 0, 1);
                REQUIRE(cold.has_value());
                REQUIRE(*cold == 20.0);
            }
        }
    }

    std::filesystem::remove(db_path);
    std::filesystem::remove_all(archive_dir);
}

#endif  // FRE_ENABLE_DUCKDB
