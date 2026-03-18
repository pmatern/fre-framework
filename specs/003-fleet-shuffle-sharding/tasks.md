# Tasks: Fleet-Level Shuffle Sharding

**Input**: Design documents from `/specs/003-fleet-shuffle-sharding/`
**Status**: ALL TASKS COMPLETE (retroactive documentation — implemented 2026-03-17)

## Phase 1: Setup (Shared Infrastructure)

- [x] T001 Extract `k_hash_seeds[]` to `include/fre/sharding/hash_seeds.hpp`
- [x] T002 Update `src/sharding/tenant_router.cpp` to `#include <fre/sharding/hash_seeds.hpp>` (remove inline definition)
- [x] T003 Add `FleetRoutingError` struct/enum to `include/fre/core/error.hpp` (`NotOwner`, `TopologyUnavailable` codes)
- [x] T004 Implement `FleetRoutingError::message()` in `src/pipeline/error.cpp`

---

## Phase 2: Foundational

- [x] T005 Define `InstanceInfo`, `FleetConfig`, `FleetRouter` class in `include/fre/service/fleet_router.hpp`
- [x] T006 Implement `assign_instances()` and `FleetRouter` methods in `service/src/fleet_router.cpp`
- [x] T007 Add `src/fleet_router.cpp` to `fre_service_lib` in `service/CMakeLists.txt`

**Checkpoint**: FleetRouter compilable and linkable ✅

---

## Phase 3: User Story 1 — Deterministic Tenant Ownership (P1) 🎯 MVP

**Goal**: `owns()` returns true for exactly K instances out of N for any tenant.

### Tests for User Story 1

- [x] T008 [P] [US1] Unit test: determinism — `test_fleet_router.cpp` "owns() is deterministic"
- [x] T009 [P] [US1] Unit test: single-instance fleet owns all — `test_fleet_router.cpp`
- [x] T010 [P] [US1] Unit test: fleet sweep — exactly `instances_per_tenant` owners — `test_fleet_router.cpp`
- [x] T011 [P] [US1] Unit test: isolation property (fleet_size=31, K=4, <20 collision pairs) — `test_fleet_router.cpp`

### Implementation for User Story 1

*(Covered by Phase 2 foundational tasks — FleetRouter::owns() is the core deliverable)*

**Checkpoint**: Fleet ownership logic fully verified ✅

---

## Phase 4: User Story 2 — Redirect Hints for Non-Owners (P2)

**Goal**: Non-owner returns HTTP 503 + `X-Fre-Redirect-Hint`; owner returns HTTP 202.

### Tests for User Story 2

- [x] T012 [P] [US2] Unit test: `redirect_hint()` non-empty for non-owner — `test_fleet_router.cpp`
- [x] T013 [P] [US2] Unit test: `redirect_hint()` empty when topology absent — `test_fleet_router.cpp`
- [x] T014 [US2] Integration test: non-owner → 503 + hint header — `test_fleet_routing.cpp`
- [x] T015 [US2] Integration test: owner → 202 — `test_fleet_routing.cpp`

### Implementation for User Story 2

- [x] T016 [US2] Add `std::optional<FleetConfig> fleet_config` to `HarnessConfig` in `harness.hpp`
- [x] T017 [US2] Add `std::optional<FleetRouter> fleet_router` to `Impl` in `harness.cpp`
- [x] T018 [US2] Add fleet gate (503 + header) before `pipeline.submit()` in `harness.cpp` `POST /events` handler

**Checkpoint**: Non-owner rejection and redirect hints working ✅

---

## Phase 5: User Story 3 — Topology Endpoint (P3)

**Goal**: `GET /topology` returns JSON fleet metadata.

### Tests for User Story 3

- [x] T019 [P] [US3] Unit test: `topology_json()` contains instance_id and fleet_size — `test_fleet_router.cpp`
- [x] T020 [US3] Integration test: `GET /topology` → 200 + `fleet_size` field — `test_fleet_routing.cpp`
- [x] T021 [US3] Integration test: `GET /topology` with fleet disabled → 200 + `disabled` — `test_fleet_routing.cpp`

### Implementation for User Story 3

- [x] T022 [US3] Add `GET /topology` handler to `harness.cpp`
- [x] T023 [US3] Implement `FleetRouter::topology_json()` in `fleet_router.cpp`

**Checkpoint**: Topology endpoint functional ✅

---

## Phase 6: Operations & Integration

- [x] T024 Implement `load_topology()` JSON parser in `service/src/main.cpp`
- [x] T025 Read `FRE_INSTANCE_ID`, `FRE_FLEET_SIZE`, `FRE_INSTANCES_PER_TENANT`, `FRE_TOPOLOGY_FILE` env vars in `main.cpp`
- [x] T026 [P] Register `fre_unit_fleet_router` test target in `tests/unit/CMakeLists.txt`
- [x] T027 [P] Register `fre_integration_fleet_routing` test target in `tests/integration/CMakeLists.txt`

## Phase 7: Resiliency Gate — Latency Benchmark

*Required by Constitution Principle VI / Quality Gate 6.*

- [ ] T028 [US1] `BENCHMARK("FleetRouter::owns P99")` — assert < 1µs in `tests/unit/test_fleet_router.cpp`; also add a 503-path benchmark asserting redirect overhead < 1ms

---

## Dependencies & Execution Order

- T001–T004 (setup) → T005–T007 (foundational) → T008–T023 (user stories) → T024–T027 (ops) → T028 (benchmark)
- T008–T013 and T019 (unit tests) can run in parallel
- T014–T015 and T020–T021 (integration tests) require harness changes (T016–T018, T022)
