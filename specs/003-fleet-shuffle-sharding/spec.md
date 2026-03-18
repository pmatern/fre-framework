# Feature Specification: Fleet-Level Shuffle Sharding

**Feature Branch**: `003-fleet-shuffle-sharding`
**Created**: 2026-03-17
**Status**: Implemented
**Input**: Apply the same Vogels 2014 combinatorial hash used internally by `TenantRouter` one level higher so each `fre-service` instance in a multi-instance fleet accepts only its deterministic subset of tenants. A noisy tenant can saturate its K owner instances without impacting tenants assigned to other instances.

## Clarifications

### Session 2026-03-17

- Q: Should fleet routing be backward-compatible with single-instance deployments? → A: Yes — when `fleet_config` is absent all tenants are accepted unchanged.
- Q: How are owner addresses communicated to callers on rejection? → A: Via a `X-Fre-Redirect-Hint` response header listing `host:port` pairs, computed deterministically from a static topology file; no network call needed.
- Q: Must the fleet hash use the same seeds as the intra-instance cell hash? → A: Yes — seeds extracted to a shared header; any divergence would silently misroute traffic.

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Deterministic Tenant Ownership (Priority: P1)

Each `fre-service` instance deterministically owns exactly `instances_per_tenant` out of `fleet_size` instances for any given tenant. The set of owners is identical regardless of which instance computes it, and does not change across restarts.

**Why this priority**: This is the foundational isolation guarantee. Without it no blast-radius reduction is possible.

**Independent Test**: Sweep all `fleet_size` instances, count how many call `owns(tenant_id) == true`, assert count equals `instances_per_tenant`.

**Acceptance Scenarios**:

1. **Given** a fleet of N instances and `instances_per_tenant=K`, **When** every instance evaluates `owns(tenant_id)` for the same tenant, **Then** exactly K instances return true and N-K return false.
2. **Given** any two calls to `owns(tenant_id)` on the same `FleetRouter` instance, **Then** both calls return the same result (determinism).
3. **Given** a single-instance fleet (`fleet_size=1`), **When** `owns()` is called for any tenant, **Then** it always returns true.

---

### User Story 2 — Redirect Hints for Non-Owners (Priority: P2)

When a non-owner instance receives a tenant's event it returns HTTP 503 with an `X-Fre-Redirect-Hint` header listing the addresses of the actual owner instances, derived from the static topology file.

**Why this priority**: Without hints the caller cannot recover; it would need to retry round-robin until it hits an owner by chance.

**Independent Test**: Configure two in-process `ServiceHarness` instances (fleet_size=2, instances_per_tenant=1). Submit a tenant owned by instance 0 to instance 1; assert 503 + header containing instance 0's address.

**Acceptance Scenarios**:

1. **Given** an event for a tenant not owned by this instance, **When** `POST /events` is called, **Then** the response is HTTP 503 and the `X-Fre-Redirect-Hint` header lists owner addresses.
2. **Given** an event for a tenant owned by this instance, **When** `POST /events` is called, **Then** the response is HTTP 202.
3. **Given** a `FleetConfig` with no topology entries, **When** `redirect_hint()` is called, **Then** it returns an empty string (no crash).

---

### User Story 3 — Topology Endpoint (Priority: P3)

`GET /topology` returns JSON describing the fleet configuration of this instance, enabling upstream proxies and service-discovery agents to build routing tables.

**Why this priority**: Supports operational visibility; upstream LBs can cache the topology rather than relying solely on redirect hints.

**Acceptance Scenarios**:

1. **Given** a fleet-enabled harness, **When** `GET /topology` is requested, **Then** the response is 200 JSON containing `fleet_size`, `instance_id`, and `instances_per_tenant`.
2. **Given** a harness with `fleet_config = nullopt`, **When** `GET /topology` is requested, **Then** the response is 200 JSON containing `"fleet_routing":"disabled"`.

---

### Edge Cases

- `fleet_size` that is a power of 2: `% fleet_size` uses only low bits of the hash — produces fewer distinct owner sets. Non-power-of-2 sizes required for strong isolation.
- `instances_per_tenant >= fleet_size`: every instance owns every tenant (degenerate case — equivalent to no fleet routing).
- Hash collision within a tenant's K-set: `assign_instances()` deduplicates and continues the seed sequence.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: `FleetRouter::owns(tenant_id)` MUST return true for exactly `instances_per_tenant` out of `fleet_size` instances for any given tenant.
- **FR-002**: The ownership assignment MUST use the same hash seeds as `TenantRouter::assign_cells()` (defined in `include/fre/sharding/hash_seeds.hpp`).
- **FR-003**: When `fleet_config` is absent from `HarnessConfig`, ALL tenants MUST be accepted without fleet routing.
- **FR-004**: Non-owner instances MUST respond to `POST /events` with HTTP 503, a `X-Fre-Redirect-Hint` header whose value is a comma-separated list of owner `host:port` addresses (from the topology), and a JSON body `{"error":"not_owner","redirect_hint":"<value>"}`. When topology is empty the header MUST be omitted entirely (not sent with an empty value).
- **FR-005**: `redirect_hint()` MUST derive owner addresses from the static topology vector without any network call.
- **FR-006**: `GET /topology` MUST return JSON with `fleet_size`, `instance_id`, `instances_per_tenant` when fleet is enabled.
- **FR-007**: `GET /topology` MUST return JSON with `"fleet_routing":"disabled"` when fleet is absent.
- **FR-008**: Fleet configuration MUST be loadable from env vars `FRE_INSTANCE_ID`, `FRE_FLEET_SIZE`, `FRE_INSTANCES_PER_TENANT`, `FRE_TOPOLOGY_FILE`.

### Key Entities

- **`FleetConfig`**: `instance_id`, `fleet_size`, `instances_per_tenant`, `topology: vector<InstanceInfo>`.
- **`InstanceInfo`**: `{id: uint32_t, address: string}` — `host:port` for topology hints.
- **`FleetRouter`**: Owns `FleetConfig`; exposes `owns()`, `redirect_hint()`, `topology_json()`.
- **`FleetRoutingError`**: Added to `fre::Error` variant with codes `NotOwner`, `TopologyUnavailable`.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: For any tenant across a fleet sweep, exactly `instances_per_tenant` instances return `owns() == true`.
- **SC-002**: With `fleet_size=31, instances_per_tenant=4`, fewer than 20 out of 19,900 tenant pairs share identical owner sets (isolation property; expected ~0.6 by combinatorics).
- **SC-003**: Non-owner HTTP 503 response includes `X-Fre-Redirect-Hint` containing the correct owner's `host:port`.
- **SC-004**: Owner HTTP 202 response is returned within the existing pipeline latency budget.
- **SC-005**: All existing tests (001, 002) continue to pass with no modification (no regression).

## Assumptions

- Fleet topology is static at startup; no dynamic membership changes during a process lifetime.
- Upstream LBs use redirect hints for retry routing; the framework does not proxy requests.
- Hash seed stability is guaranteed by extracting seeds to a shared header — seeds MUST NOT be changed post-deployment.
