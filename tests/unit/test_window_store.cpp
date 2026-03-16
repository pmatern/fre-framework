/// T043 — Unit tests for InProcessWindowStore.

#include <fre/state/window_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <vector>

using namespace fre;
using namespace std::chrono_literals;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static WindowKey make_key(std::string entity, uint64_t epoch = 0) {
    return WindowKey{
        .tenant_id   = "acme",
        .entity_id   = std::move(entity),
        .window_name = "test_window",
        .epoch       = epoch,
    };
}

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("InProcessWindowStore: get returns zero value for missing key", "[unit][window_store][US3]") {
    InProcessWindowStore store;
    auto result = store.get(make_key("user-1"));
    REQUIRE(result.has_value());
    REQUIRE(result->aggregate == 0.0);
    REQUIRE(result->version == 0);
}

TEST_CASE("InProcessWindowStore: compare_and_swap atomically increments", "[unit][window_store][US3]") {
    InProcessWindowStore store;
    const auto key = make_key("user-2");

    // First CAS: from zero to 1
    WindowValue expected{.aggregate = 0.0, .version = 0};
    WindowValue next{.aggregate = 1.0, .version = 1};
    auto result = store.compare_and_swap(key, expected, next);
    REQUIRE(result.has_value());
    REQUIRE(*result == true);

    // Read back
    auto read = store.get(key);
    REQUIRE(read.has_value());
    REQUIRE(read->aggregate == 1.0);
    REQUIRE(read->version == 1);

    // Stale CAS (wrong version) should fail
    WindowValue stale_expected{.aggregate = 0.0, .version = 0};
    WindowValue stale_next{.aggregate = 5.0, .version = 1};
    auto stale_result = store.compare_and_swap(key, stale_expected, stale_next);
    REQUIRE(stale_result.has_value());
    REQUIRE(*stale_result == false);

    // Value should be unchanged
    auto read2 = store.get(key);
    REQUIRE(read2->aggregate == 1.0);
}

TEST_CASE("InProcessWindowStore: expiry removes key", "[unit][window_store][US3]") {
    InProcessWindowStore store;
    const auto key = make_key("user-3");

    // Insert
    store.compare_and_swap(key, WindowValue{}, WindowValue{.aggregate = 42.0, .version = 1});

    auto before_expire = store.get(key);
    REQUIRE(before_expire->aggregate == 42.0);

    // Expire
    auto expire_result = store.expire(key);
    REQUIRE(expire_result.has_value());

    // After expiry, should return zero
    auto after_expire = store.get(key);
    REQUIRE(after_expire.has_value());
    REQUIRE(after_expire->aggregate == 0.0);
}

TEST_CASE("InProcessWindowStore: concurrent increments reach expected count", "[unit][window_store][US3]") {
    InProcessWindowStore store;
    const auto key = make_key("user-concurrent");

    constexpr int k_num_threads   = 8;
    constexpr int k_ops_per_thread = 50;

    std::vector<std::thread> threads;
    threads.reserve(k_num_threads);

    for (int t = 0; t < k_num_threads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < k_ops_per_thread; ++i) {
                bool swapped = false;
                for (int attempts = 0; attempts < 20 && !swapped; ++attempts) {
                    auto cur = store.get(key);
                    if (!cur.has_value()) break;
                    WindowValue next{.aggregate = cur->aggregate + 1.0, .version = cur->version + 1};
                    auto cas = store.compare_and_swap(key, *cur, next);
                    swapped = cas.has_value() && *cas;
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    auto final_val = store.get(key);
    REQUIRE(final_val.has_value());
    // All k_num_threads * k_ops_per_thread increments should have landed
    REQUIRE(final_val->aggregate == static_cast<double>(k_num_threads * k_ops_per_thread));
}

TEST_CASE("InProcessWindowStore: different epochs are independent", "[unit][window_store][US3]") {
    InProcessWindowStore store;

    auto key_epoch0 = make_key("user-epoch", 0);
    auto key_epoch1 = make_key("user-epoch", 1);

    store.compare_and_swap(key_epoch0, WindowValue{}, WindowValue{.aggregate = 10.0, .version = 1});
    store.compare_and_swap(key_epoch1, WindowValue{}, WindowValue{.aggregate = 20.0, .version = 1});

    auto val0 = store.get(key_epoch0);
    auto val1 = store.get(key_epoch1);

    REQUIRE(val0.has_value());
    REQUIRE(val1.has_value());
    REQUIRE(val0->aggregate == 10.0);
    REQUIRE(val1->aggregate == 20.0);
}

TEST_CASE("InProcessWindowStore: expiry callback fires on expire", "[unit][window_store][US3]") {
    InProcessWindowStore store;

    bool callback_fired = false;
    WindowKey  fired_key;
    WindowValue fired_val;

    store.register_expiry_callback([&](const WindowKey& k, const WindowValue& v) {
        callback_fired = true;
        fired_key      = k;
        fired_val      = v;
    });

    const auto key = make_key("user-cb");
    store.compare_and_swap(key, WindowValue{}, WindowValue{.aggregate = 7.0, .version = 1});
    store.expire(key);

    REQUIRE(callback_fired);
    REQUIRE(fired_key.entity_id == "user-cb");
    REQUIRE(fired_val.aggregate == 7.0);
}
