#ifdef FRE_ENABLE_DUCKDB

/// Unit tests for DuckDbWindowStore.

#include <fre/state/duckdb_window_store.hpp>
#include <fre/state/external_store.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace fre;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static WindowKey make_key(std::string entity, uint64_t epoch = 0) {
    return WindowKey{
        .tenant_id   = "acme",
        .entity_id   = std::move(entity),
        .window_name = "test_window",
        .epoch       = epoch,
    };
}

static DuckDbWindowStore in_memory_store() {
    return DuckDbWindowStore{DuckDbConfig{
        .db_path              = "",  // in-memory
        .parquet_archive_dir  = "",
        .flush_interval_ms    = 0,   // disable flush thread
        .window_ms            = 60000,
        .warm_epoch_retention = 3,
    }};
}

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("DuckDbWindowStore: is_available true after successful open", "[duckdb][unit]") {
    auto store = in_memory_store();
    REQUIRE(store.is_available());
}

TEST_CASE("DuckDbWindowStore: get returns zero for missing key", "[duckdb][unit]") {
    auto store = in_memory_store();
    auto result = store.get(make_key("missing-entity"));
    REQUIRE(result.has_value());
    REQUIRE(result->aggregate == 0.0);
    REQUIRE(result->version == 0);
}

TEST_CASE("DuckDbWindowStore: compare_and_swap inserts and updates correctly", "[duckdb][unit]") {
    auto store = in_memory_store();
    const auto key = make_key("user-cas");

    SECTION("first CAS from version 0 succeeds") {
        WindowValue expected{.aggregate = 0.0, .version = 0};
        WindowValue next{.aggregate = 1.0, .version = 1};
        auto result = store.compare_and_swap(key, expected, next);
        REQUIRE(result.has_value());
        REQUIRE(*result == true);

        auto read = store.get(key);
        REQUIRE(read.has_value());
        REQUIRE(read->aggregate == 1.0);
        REQUIRE(read->version == 1);
    }

    SECTION("stale CAS (wrong version) returns false") {
        // Set initial value
        WindowValue v0{.aggregate = 0.0, .version = 0};
        WindowValue v1{.aggregate = 5.0, .version = 1};
        store.compare_and_swap(key, v0, v1);

        // Attempt with stale expected version
        WindowValue stale_expected{.aggregate = 0.0, .version = 0};
        WindowValue stale_next{.aggregate = 99.0, .version = 1};
        auto stale = store.compare_and_swap(key, stale_expected, stale_next);
        REQUIRE(stale.has_value());
        REQUIRE(*stale == false);

        // Value unchanged
        auto read = store.get(key);
        REQUIRE(read.has_value());
        REQUIRE(read->aggregate == 5.0);
    }

    SECTION("successive CAS increments version") {
        WindowValue v0{.aggregate = 0.0, .version = 0};
        WindowValue v1{.aggregate = 1.0, .version = 1};
        WindowValue v2{.aggregate = 2.0, .version = 2};

        auto r1 = store.compare_and_swap(key, v0, v1);
        REQUIRE(r1.has_value());
        REQUIRE(*r1 == true);

        auto r2 = store.compare_and_swap(key, v1, v2);
        REQUIRE(r2.has_value());
        REQUIRE(*r2 == true);

        auto read = store.get(key);
        REQUIRE(read->aggregate == 2.0);
        REQUIRE(read->version == 2);
    }
}

TEST_CASE("DuckDbWindowStore: expire removes the row", "[duckdb][unit]") {
    auto store = in_memory_store();
    const auto key = make_key("user-expire");

    // Insert
    store.compare_and_swap(key, WindowValue{}, WindowValue{.aggregate = 42.0, .version = 1});
    auto before = store.get(key);
    REQUIRE(before->aggregate == 42.0);

    // Expire
    auto expire_result = store.expire(key);
    REQUIRE(expire_result.has_value());

    // After expiry, returns zero (missing key behaviour)
    auto after = store.get(key);
    REQUIRE(after.has_value());
    REQUIRE(after->aggregate == 0.0);
    REQUIRE(after->version == 0);
}

TEST_CASE("DuckDbWindowStore: different epochs are independent", "[duckdb][unit]") {
    auto store = in_memory_store();

    const auto key0 = make_key("user-epoch", 0);
    const auto key1 = make_key("user-epoch", 1);

    store.compare_and_swap(key0, WindowValue{}, WindowValue{.aggregate = 10.0, .version = 1});
    store.compare_and_swap(key1, WindowValue{}, WindowValue{.aggregate = 20.0, .version = 1});

    REQUIRE(store.get(key0)->aggregate == 10.0);
    REQUIRE(store.get(key1)->aggregate == 20.0);
}

TEST_CASE("DuckDbWindowStore: query_range sums across warm tier epochs", "[duckdb][unit]") {
    auto store = in_memory_store();

    // Write 3 values across 3 epochs
    for (uint64_t ep = 0; ep < 3; ++ep) {
        const auto key = WindowKey{.tenant_id = "acme", .entity_id = "user-q",
                                    .window_name = "counts", .epoch = ep};
        store.compare_and_swap(key, WindowValue{}, WindowValue{.aggregate = 10.0, .version = 1});
    }

    auto result = store.query_range("acme", "user-q", "counts", 0, 2);
    REQUIRE(result.has_value());
    REQUIRE(*result == 30.0);

    // Partial range
    auto partial = store.query_range("acme", "user-q", "counts", 1, 2);
    REQUIRE(partial.has_value());
    REQUIRE(*partial == 20.0);
}

TEST_CASE("DuckDbWindowStore: as_backend satisfies ExternalStoreBackend", "[duckdb][unit]") {
    auto store   = in_memory_store();
    auto backend = store.as_backend();
    auto fallback = std::make_shared<InProcessWindowStore>();

    ExternalWindowStore ext{std::move(backend), fallback};
    REQUIRE(ext.is_available());

    const auto key = make_key("backend-user");
    auto result = ext.get(key);
    REQUIRE(result.has_value());
    REQUIRE(result->aggregate == 0.0);
}

TEST_CASE("DuckDbWindowStore: parquet_archive_dir can be configured without errors", "[duckdb][unit]") {
    const std::string archive_dir = "/tmp/fre_duckdb_test_archive_" +
                                    std::to_string(std::hash<std::string>{}("unit-test"));
    std::filesystem::remove_all(archive_dir);

    DuckDbWindowStore store{DuckDbConfig{
        .db_path             = "",    // in-memory
        .parquet_archive_dir = archive_dir,
        .flush_interval_ms   = 0,    // disable background flush thread
        .window_ms           = 1000,
        .warm_epoch_retention = 1,
    }};

    REQUIRE(store.is_available());

    // Write to epoch 0 and read back via query_range
    const WindowKey key{.tenant_id = "t1", .entity_id = "e1",
                         .window_name = "w1", .epoch = 0};
    store.compare_and_swap(key, WindowValue{}, WindowValue{.aggregate = 5.0, .version = 1});

    auto result = store.query_range("t1", "e1", "w1", 0, 0);
    REQUIRE(result.has_value());
    REQUIRE(*result == 5.0);

    std::filesystem::remove_all(archive_dir);
}

// ─── Resiliency gate: CAS P99 latency benchmark ───────────────────────────────
// Run with: ctest --preset duckdb -R duckdb_benchmark -- --enable-benchmarking
// Target: < 10ms per CAS operation (constitution Principle VI / SC-001).

TEST_CASE("DuckDbWindowStore: CAS P99 latency benchmark", "[duckdb][benchmark]") {
    auto store = in_memory_store();
    const auto key = make_key("bench-entity");

    uint64_t version = 0;

    BENCHMARK("DuckDB CAS roundtrip") {
        const WindowValue old_val{.aggregate = static_cast<double>(version),
                                  .version   = version};
        const WindowValue new_val{.aggregate = static_cast<double>(version + 1),
                                  .version   = version + 1};
        auto result = store.compare_and_swap(key, old_val, new_val);
        if (result.has_value() && *result) ++version;
        return result;
    };
}

#endif  // FRE_ENABLE_DUCKDB
