#include <fre/service/fleet_router.hpp>
#include <fre/sharding/hash_seeds.hpp>

#include <algorithm>
#include <functional>
#include <sstream>
#include <vector>

namespace fre::service {

namespace {

// ─── assign_instances ────────────────────────────────────────────────────────
//
// Identical algorithm to assign_cells() in tenant_router.cpp — uses the same
// fre::k_hash_seeds and the same formula so that fleet-level and cell-level
// shard assignments are always computed the same way.

[[nodiscard]] std::vector<uint32_t> assign_instances(
    std::string_view tenant_id, uint32_t fleet_size, uint32_t instances_per_tenant)
{
    std::vector<uint32_t> result;
    result.reserve(instances_per_tenant);

    const std::size_t h = std::hash<std::string_view>{}(tenant_id);

    for (uint32_t i = 0; result.size() < instances_per_tenant; ++i) {
        const uint32_t seed     = (i < std::size(fre::k_hash_seeds))
                                      ? fre::k_hash_seeds[i]
                                      : (i * 0x9e3779b9u);
        const uint32_t instance = static_cast<uint32_t>((h ^ (seed + i)) % fleet_size);

        if (std::find(result.begin(), result.end(), instance) == result.end()) {
            result.push_back(instance);
        }
        if (result.size() == fleet_size) break; // can't get more unique instances
    }

    return result;
}

}  // namespace

// ─── FleetRouter ─────────────────────────────────────────────────────────────

FleetRouter::FleetRouter(FleetConfig config) : config_{std::move(config)} {}

bool FleetRouter::owns(std::string_view tenant_id) const noexcept {
    if (config_.fleet_size <= 1) return true;

    const auto owners = assign_instances(
        tenant_id, config_.fleet_size, config_.instances_per_tenant);

    return std::find(owners.begin(), owners.end(), config_.instance_id) != owners.end();
}

std::string FleetRouter::redirect_hint(std::string_view tenant_id) const {
    if (config_.fleet_size <= 1 || config_.topology.empty()) return {};

    const auto owner_ids = assign_instances(
        tenant_id, config_.fleet_size, config_.instances_per_tenant);

    std::ostringstream oss;
    bool first = true;
    for (uint32_t id : owner_ids) {
        for (const auto& info : config_.topology) {
            if (info.id == id && !info.address.empty()) {
                if (!first) oss << ',';
                oss << info.address;
                first = false;
                break;
            }
        }
    }
    return oss.str();
}

std::string FleetRouter::topology_json() const {
    std::ostringstream oss;
    oss << "{"
        << "\"instance_id\":" << config_.instance_id << ","
        << "\"fleet_size\":" << config_.fleet_size << ","
        << "\"instances_per_tenant\":" << config_.instances_per_tenant
        << "}";
    return oss.str();
}

}  // namespace fre::service
