# Implementation Plan: Fleet-Level Shuffle Sharding

**Branch**: `003-fleet-shuffle-sharding` | **Date**: 2026-03-17 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/003-fleet-shuffle-sharding/spec.md`

## Summary

Apply the Vogels 2014 combinatorial shuffle-shard hash (already used by `TenantRouter` for cell assignment) one level higher so each `fre-service` instance in an N-instance fleet owns exactly K tenants. Non-owners return HTTP 503 + `X-Fre-Redirect-Hint`. A new `GET /topology` endpoint exposes fleet metadata. Fleet routing is opt-in via env vars; all existing behaviour is preserved when unconfigured.

## Technical Context

**Language/Version**: C++23 — GCC 14+ / Clang 18+
**Primary Dependencies**: Standalone Asio (existing), no new dependencies
**Storage**: Static topology JSON file (loaded at startup)
**Testing**: Catch2 v3 — unit + integration
**Target Platform**: Linux server
**Project Type**: Library + service harness
**Performance Goals**: `owns()` call < 1µs; 503 response overhead < 1ms
**Constraints**: Zero new runtime dependencies; backward-compatible when fleet_config absent
**Scale/Scope**: Fleet sizes tested up to 31 instances; instances_per_tenant up to 4

## Constitution Check

| Gate | Status | Notes |
|------|--------|-------|
| Spec gate | ✅ | spec.md with acceptance scenarios |
| Test gate | ✅ | Unit + integration tests written alongside implementation |
| Dependency gate | ✅ | No new external dependencies |
| Versioning gate | ✅ | MINOR bump — new additive API, fully backward-compatible |
| Simplicity gate | ✅ | Reuses existing hash algorithm; minimal new code surface |
| Resiliency gate | ✅ | Failure mode: topology absent → empty hint (no crash); fleet absent → accept all |

## Failure Mode Analysis

| Failure | Blast Radius | Degradation Strategy |
|---------|-------------|---------------------|
| `FRE_TOPOLOGY_FILE` missing | This instance only | `redirect_hint()` returns empty string; 503 still issued so caller can retry |
| Hash seed divergence | Full fleet misrouting | Seeds in shared `hash_seeds.hpp` — single canonical definition, never duplicated |
| `fleet_size` is power-of-2 | Weak isolation | Documented limitation; operators must use non-power-of-2 for strong isolation |
| `instances_per_tenant >= fleet_size` | No isolation (all own all) | Degenerate but safe; degrades to full-acceptance mode |

## Project Structure

### Documentation (this feature)

```text
specs/003-fleet-shuffle-sharding/
├── plan.md              # This file
├── spec.md              # Feature specification
├── tasks.md             # Task list (all complete)
└── contracts/
    └── fleet-router-contract.md
```

### Source Code

```text
include/fre/sharding/hash_seeds.hpp          # NEW: shared seed constants
include/fre/service/fleet_router.hpp         # NEW: FleetConfig, InstanceInfo, FleetRouter
include/fre/core/error.hpp                   # MODIFIED: FleetRoutingError added
service/src/fleet_router.cpp                 # NEW: assign_instances() + FleetRouter impl
service/src/harness.cpp                      # MODIFIED: fleet gate + GET /topology
service/src/main.cpp                         # MODIFIED: env var loading
service/include/fre/service/harness.hpp      # MODIFIED: fleet_config field
service/CMakeLists.txt                       # MODIFIED: fleet_router.cpp added
src/sharding/tenant_router.cpp               # MODIFIED: uses shared hash_seeds.hpp
src/pipeline/error.cpp                       # MODIFIED: FleetRoutingError::message()
tests/unit/test_fleet_router.cpp             # NEW: unit tests
tests/integration/test_fleet_routing.cpp     # NEW: integration tests
tests/unit/CMakeLists.txt                    # MODIFIED
tests/integration/CMakeLists.txt             # MODIFIED
```

## Complexity Tracking

| Decision | Why Needed | Simpler Alternative Rejected Because |
|----------|-----------|-------------------------------------|
| Shared `hash_seeds.hpp` | Guarantees fleet and cell hashes are bit-identical | Duplicating the array risks silent divergence if one copy is updated |
| Static topology file (no gossip) | Deterministic redirect hints without network calls | Dynamic membership adds operational complexity and a liveness dependency |
| `std::optional<FleetConfig>` in HarnessConfig | Backward compatibility with no fleet config | Separate HarnessConfig subtype would break existing harness construction sites |
