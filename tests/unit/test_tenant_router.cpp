#include <fre/sharding/tenant_router.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace fre;

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TenantRouter: cell assignment is deterministic and stable", "[sharding][tenant_router]") {
    TenantRouter router{
        ShardingConfig{.num_cells = 16, .cells_per_tenant = 4, .thread_count = 2},
        RateLimitConfig{},
    };

    GIVEN("the same tenant_id queried twice") {
        auto cells1 = router.cells_for("acme");
        auto cells2 = router.cells_for("acme");

        THEN("both results have K=4 cells") {
            REQUIRE(cells1.size() == 4);
            REQUIRE(cells2.size() == 4);
        }

        THEN("cell pointers are identical (same assignment)") {
            REQUIRE(cells1.size() == cells2.size());
            for (std::size_t i = 0; i < cells1.size(); ++i) {
                REQUIRE(cells1[i] == cells2[i]);
            }
        }
    }
}

TEST_CASE("TenantRouter: cell assignments contain no duplicate strands", "[sharding][tenant_router]") {
    TenantRouter router{
        ShardingConfig{.num_cells = 16, .cells_per_tenant = 4, .thread_count = 2},
        RateLimitConfig{},
    };

    auto cells = router.cells_for("tenant-uniqueness-test");
    std::set<TenantRouter::Strand*> unique_ptrs(cells.begin(), cells.end());
    REQUIRE(unique_ptrs.size() == cells.size());
}

TEST_CASE("TenantRouter: different tenants likely get different cell sets", "[sharding][tenant_router]") {
    // With K=4, N=16, P(two tenants share ALL 4 cells) ≈ 0.07%.
    // We just verify they don't always get the exact same set across many tenants.
    TenantRouter router{
        ShardingConfig{.num_cells = 16, .cells_per_tenant = 4, .thread_count = 2},
        RateLimitConfig{},
    };

    const std::vector<std::string> tenants = {
        "tenant-a", "tenant-b", "tenant-c", "tenant-d", "tenant-e",
    };

    std::vector<std::vector<TenantRouter::Strand*>> all_cells;
    for (const auto& t : tenants) {
        auto span = router.cells_for(t);
        all_cells.emplace_back(span.begin(), span.end());
    }

    // At least one pair of tenants should differ in their cell set
    bool found_difference = false;
    for (std::size_t i = 0; i < all_cells.size() && !found_difference; ++i) {
        for (std::size_t j = i + 1; j < all_cells.size() && !found_difference; ++j) {
            if (all_cells[i] != all_cells[j]) found_difference = true;
        }
    }
    REQUIRE(found_difference);
}

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TenantRouter: token bucket allows burst up to capacity then rejects", "[sharding][rate_limit]") {
    TenantRouter router{
        ShardingConfig{.num_cells = 16, .cells_per_tenant = 4, .thread_count = 2},
        RateLimitConfig{
            .bucket_capacity  = 10,
            .tokens_per_second = 1,  // slow refill so bucket stays empty
            .max_concurrent   = 1000,
        },
    };

    // First 10 acquisitions should succeed
    for (int i = 0; i < 10; ++i) {
        auto result = router.try_acquire("burst-tenant");
        INFO("Iteration " << i);
        REQUIRE(result.has_value());
        router.release("burst-tenant");
    }

    // Drain the bucket with 10 more acquisitions (no releases yet)
    for (int i = 0; i < 10; ++i) {
        (void)router.try_acquire("burst-tenant");
    }

    // Next acquisition should be rejected
    auto over_limit = router.try_acquire("burst-tenant");
    REQUIRE_FALSE(over_limit.has_value());
    REQUIRE(over_limit.error().code == RateLimitErrorCode::Exhausted);
}

TEST_CASE("TenantRouter: concurrency cap rejects when in-flight exceeds max", "[sharding][rate_limit]") {
    TenantRouter router{
        ShardingConfig{.num_cells = 16, .cells_per_tenant = 4, .thread_count = 2},
        RateLimitConfig{
            .bucket_capacity   = 1000,
            .tokens_per_second = 10000,
            .max_concurrent    = 3,
        },
    };

    // Acquire max_concurrent slots without releasing
    for (int i = 0; i < 3; ++i) {
        auto result = router.try_acquire("cap-tenant");
        INFO("Acquiring slot " << i);
        REQUIRE(result.has_value());
    }

    // One more should hit the concurrency cap
    auto over_cap = router.try_acquire("cap-tenant");
    REQUIRE_FALSE(over_cap.has_value());
    REQUIRE(over_cap.error().code == RateLimitErrorCode::ConcurrencyCapReached);

    // After releasing one, should succeed again
    router.release("cap-tenant");
    auto after_release = router.try_acquire("cap-tenant");
    REQUIRE(after_release.has_value());
}

TEST_CASE("TenantRouter: rate limits for one tenant do not affect another", "[sharding][isolation]") {
    TenantRouter router{
        ShardingConfig{.num_cells = 16, .cells_per_tenant = 4, .thread_count = 2},
        RateLimitConfig{
            .bucket_capacity   = 5,
            .tokens_per_second = 1,
            .max_concurrent    = 1000,
        },
    };

    // Exhaust tenant-A's bucket
    for (int i = 0; i < 5; ++i) {
        (void)router.try_acquire("tenant-A");
    }
    auto a_over = router.try_acquire("tenant-A");
    REQUIRE_FALSE(a_over.has_value());

    // tenant-B should still have a full bucket
    auto b_result = router.try_acquire("tenant-B");
    REQUIRE(b_result.has_value());
}
