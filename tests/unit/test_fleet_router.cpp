/// Unit tests for FleetRouter — fleet-level shuffle shard ownership.

#include <fre/service/fleet_router.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

using namespace fre::service;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static FleetConfig make_fleet(uint32_t instance_id, uint32_t fleet_size,
                               uint32_t instances_per_tenant = 2)
{
    FleetConfig cfg;
    cfg.instance_id          = instance_id;
    cfg.fleet_size           = fleet_size;
    cfg.instances_per_tenant = instances_per_tenant;
    for (uint32_t i = 0; i < fleet_size; ++i) {
        cfg.topology.push_back(InstanceInfo{i, "10.0.0." + std::to_string(i + 1) + ":8080"});
    }
    return cfg;
}

// ─── Ownership determinism ────────────────────────────────────────────────────

TEST_CASE("FleetRouter: owns() is deterministic for the same tenant_id", "[fleet_router]") {
    FleetRouter router{make_fleet(0, 8, 2)};

    REQUIRE(router.owns("acme") == router.owns("acme"));
    REQUIRE(router.owns("beta-corp") == router.owns("beta-corp"));
    REQUIRE(router.owns("tenant-xyz") == router.owns("tenant-xyz"));
}

TEST_CASE("FleetRouter: single-instance fleet owns all tenants", "[fleet_router]") {
    FleetRouter router{make_fleet(0, 1, 1)};

    REQUIRE(router.owns("any-tenant"));
    REQUIRE(router.owns("another-one"));
}

TEST_CASE("FleetRouter: fleet_size<=1 always owns regardless of instance_id", "[fleet_router]") {
    FleetConfig cfg;
    cfg.instance_id          = 0;
    cfg.fleet_size           = 1;
    cfg.instances_per_tenant = 1;
    FleetRouter router{cfg};

    REQUIRE(router.owns("some-tenant"));
}

// ─── Exactly K owners across the fleet ───────────────────────────────────────

TEST_CASE("FleetRouter: exactly instances_per_tenant instances own each tenant", "[fleet_router]") {
    constexpr uint32_t fleet_size           = 8;
    constexpr uint32_t instances_per_tenant = 2;

    const std::vector<std::string> tenants = {
        "acme", "beta", "gamma", "delta", "epsilon", "zeta",
        "tenant-007", "tenant-42", "important-customer",
    };

    for (const auto& tenant : tenants) {
        uint32_t owner_count = 0;
        for (uint32_t id = 0; id < fleet_size; ++id) {
            FleetRouter r{make_fleet(id, fleet_size, instances_per_tenant)};
            if (r.owns(tenant)) ++owner_count;
        }
        INFO("tenant: " << tenant);
        REQUIRE(owner_count == instances_per_tenant);
    }
}

// ─── Isolation property ───────────────────────────────────────────────────────

TEST_CASE("FleetRouter: different tenants have mostly non-overlapping owner sets (isolation)",
          "[fleet_router][isolation]")
{
    // Use K=4, N=31 (non-power-of-2 avoids low-bit aliasing with modulo).
    // C(31,4) = 31465 distinct owner sets. With 200 tenants, expected collision
    // pairs ≈ 200*199/(2*31465) ≈ 0.6. Threshold of 20 is generous.
    constexpr uint32_t fleet_size           = 31;
    constexpr uint32_t instances_per_tenant = 4;

    // Collect owner set (as uint64_t bitmask) for each of 200 tenants
    auto owner_mask = [&](std::string_view tenant_id) -> uint64_t {
        uint64_t mask = 0;
        for (uint32_t id = 0; id < fleet_size; ++id) {
            FleetRouter r{make_fleet(id, fleet_size, instances_per_tenant)};
            if (r.owns(tenant_id)) mask |= (uint64_t{1} << id);
        }
        return mask;
    };

    std::vector<uint64_t> masks;
    masks.reserve(200);
    for (int i = 0; i < 200; ++i) {
        masks.push_back(owner_mask("tenant-" + std::to_string(i)));
    }

    // Count pairs with identical owner sets (full overlap)
    int full_overlap_pairs = 0;
    for (std::size_t i = 0; i < masks.size(); ++i) {
        for (std::size_t j = i + 1; j < masks.size(); ++j) {
            if (masks[i] == masks[j]) ++full_overlap_pairs;
        }
    }

    REQUIRE(full_overlap_pairs < 20);
}

// ─── redirect_hint ────────────────────────────────────────────────────────────

TEST_CASE("FleetRouter: redirect_hint returns non-empty string for non-owner", "[fleet_router]") {
    constexpr uint32_t fleet_size = 8;

    // Find a tenant not owned by instance 0
    std::string non_owned_tenant;
    for (int i = 0; i < 100; ++i) {
        std::string t = "tenant-" + std::to_string(i);
        FleetRouter r{make_fleet(0, fleet_size, 2)};
        if (!r.owns(t)) {
            non_owned_tenant = t;
            break;
        }
    }
    REQUIRE(!non_owned_tenant.empty());

    FleetRouter router{make_fleet(0, fleet_size, 2)};
    const std::string hint = router.redirect_hint(non_owned_tenant);
    REQUIRE(!hint.empty());
    // Hint should contain at least one "host:port" pattern
    REQUIRE(hint.find(':') != std::string::npos);
}

TEST_CASE("FleetRouter: redirect_hint is empty when topology is not set", "[fleet_router]") {
    FleetConfig cfg;
    cfg.instance_id          = 0;
    cfg.fleet_size           = 4;
    cfg.instances_per_tenant = 2;
    // topology deliberately left empty

    FleetRouter router{cfg};
    const std::string hint = router.redirect_hint("some-tenant");
    REQUIRE(hint.empty());
}

// ─── topology_json ────────────────────────────────────────────────────────────

TEST_CASE("FleetRouter: topology_json contains instance_id and fleet_size", "[fleet_router]") {
    FleetRouter router{make_fleet(3, 8, 2)};
    const std::string json = router.topology_json();
    REQUIRE(json.find("\"instance_id\":3") != std::string::npos);
    REQUIRE(json.find("\"fleet_size\":8")  != std::string::npos);
    REQUIRE(json.find("\"instances_per_tenant\":2") != std::string::npos);
}
