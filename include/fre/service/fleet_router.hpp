#pragma once

/// Fleet-level shuffle shard router for fre-service instances.
///
/// Applies the identical combinatorial hash as TenantRouter (fre::k_hash_seeds)
/// one level higher: given a fleet of N service instances, each tenant is
/// deterministically assigned to K instances. An instance that does not own
/// a tenant rejects the request (HTTP 503) with a redirect hint pointing at
/// the owner instances.
///
/// When FleetConfig is absent (single-instance deployment), all tenants are
/// accepted unconditionally.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fre::service {

// ─── FleetConfig ─────────────────────────────────────────────────────────────

/// Describes a single instance in the service fleet.
struct InstanceInfo {
    uint32_t    id;      ///< Instance index in [0, fleet_size)
    std::string address; ///< "host:port" used in redirect hints
};

/// Configuration for fleet-level shuffle sharding.
///
/// Loaded from env vars by main():
///   FRE_INSTANCE_ID          — this instance's index (uint32, 0-based)
///   FRE_FLEET_SIZE           — total number of service instances
///   FRE_INSTANCES_PER_TENANT — instances that own each tenant (default 2)
///   FRE_TOPOLOGY_FILE        — JSON file listing {id, address} for each instance
struct FleetConfig {
    uint32_t                  instance_id{0};
    uint32_t                  fleet_size{1};
    uint32_t                  instances_per_tenant{2};
    std::vector<InstanceInfo> topology; ///< Full fleet membership (for redirect hints)
};

// ─── FleetRouter ─────────────────────────────────────────────────────────────

class FleetRouter {
public:
    explicit FleetRouter(FleetConfig config);

    /// Returns true if this instance is in the owner set for tenant_id.
    /// Always returns true when fleet_size <= 1 (single-instance mode).
    [[nodiscard]] bool owns(std::string_view tenant_id) const noexcept;

    /// Returns a comma-separated list of owner addresses from the topology
    /// (for use in the X-Fre-Redirect-Hint response header).
    /// Returns empty string when topology is not populated or fleet_size <= 1.
    [[nodiscard]] std::string redirect_hint(std::string_view tenant_id) const;

    /// Serialize this instance's fleet position as a JSON object string.
    [[nodiscard]] std::string topology_json() const;

    const FleetConfig& config() const noexcept { return config_; }

private:
    FleetConfig config_;
};

}  // namespace fre::service
