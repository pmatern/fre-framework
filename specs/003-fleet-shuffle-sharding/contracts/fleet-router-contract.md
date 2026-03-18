# Contract: FleetRouter

**Feature**: 003-fleet-shuffle-sharding
**File**: `include/fre/service/fleet_router.hpp`

## Interface

```cpp
struct InstanceInfo {
    uint32_t    id;
    std::string address;  // "host:port"
};

struct FleetConfig {
    uint32_t               instance_id{0};
    uint32_t               fleet_size{1};
    uint32_t               instances_per_tenant{2};
    std::vector<InstanceInfo> topology;  // may be empty
};

class FleetRouter {
public:
    explicit FleetRouter(FleetConfig config);

    // Returns true iff this instance is in the K-owner set for tenant_id.
    // Deterministic: same result for same (config, tenant_id) pair.
    // noexcept — never throws; safe to call on hot path.
    [[nodiscard]] bool owns(std::string_view tenant_id) const noexcept;

    // Returns comma-separated "host:port" addresses of owner instances.
    // Returns empty string when topology is empty.
    [[nodiscard]] std::string redirect_hint(std::string_view tenant_id) const;

    // Returns JSON string with fleet metadata for GET /topology.
    [[nodiscard]] std::string topology_json() const;

    const FleetConfig& config() const noexcept;
};
```

## Invariants

1. `owns()` returns `true` for exactly `min(instances_per_tenant, fleet_size)` distinct `instance_id` values in `[0, fleet_size)` for any fixed `tenant_id`.
2. `owns()` is pure (no side effects, no state mutation).
3. `redirect_hint()` lists only instances for which `owns()` would return `true`.
4. When `topology` is empty, `redirect_hint()` returns `""` (never crashes).
5. Hash seeds used in `assign_instances()` MUST match `fre::k_hash_seeds` from `hash_seeds.hpp`.

## HTTP Integration

| Condition | HTTP Response | Headers |
|-----------|--------------|---------|
| `fleet_router` absent | 202 (all accepted) | — |
| `fleet_router` present, `owns() == true` | 202 | — |
| `fleet_router` present, `owns() == false` | 503 | `X-Fre-Redirect-Hint: <hint>` |

## HarnessConfig Extension

```cpp
struct HarnessConfig {
    // ... existing fields ...
    std::optional<FleetConfig> fleet_config;  // nullopt = fleet routing disabled
};
```
