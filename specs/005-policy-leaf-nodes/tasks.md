# Tasks: Extended Policy Rule Leaf Nodes

**Input**: Design documents from `/specs/005-policy-leaf-nodes/`
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, contracts/ ✅

**Tests**: Included — constitution Principle II (TDD) is NON-NEGOTIABLE. Tests are written first
and confirmed failing before each implementation block.

**Organization**: Tasks grouped by user story. All test tasks precede their implementation tasks.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: Which user story this task belongs to (US1–US4)

---

## Phase 1: Setup

**Purpose**: Confirm baseline and register new test targets in CMake (required; test files are
not auto-discovered — each needs an explicit `fre_add_test()` entry).

- [x] T001 Verify baseline passes: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure`
- [x] T00X [P] Register unit test target in `tests/unit/CMakeLists.txt`: add `fre_add_test(TARGET fre_unit_policy_leaf_nodes SOURCES test_policy_leaf_nodes.cpp)`
- [x] T00X [P] Register integration test target in `tests/integration/CMakeLists.txt`: add `fre_add_test(TARGET fre_integration_policy_leaf_nodes SOURCES test_policy_leaf_node_integration.cpp TIMEOUT 60)`

---

## Phase 2: Foundational (Blocking Prerequisite)

**Purpose**: Add all 13 new struct declarations and expand `PolicyRule::Variant` in the header.
This must land before any test file can be compiled — all later phases depend on it.

**⚠️ CRITICAL**: No user story work can begin until T004 is complete.

- [x] T00X Add 13 new leaf node structs (`TagContains`, `TagStartsWith`, `TagIn`, `TagExists`, `TagValueLessThan`, `TagValueGreaterThan`, `TagValueBetween`, `EventTypeIs`, `EventTypeIn`, `TenantIs`, `EventOlderThan`, `EventNewerThan`, `EvaluatorScoreBetween`, `StageIsDegraded`, `EvaluatorWasSkipped`, `EvaluatorReasonIs`) and expand `PolicyRule::Variant` in `include/fre/policy/rule_engine.hpp`; add `#include <vector>` and `#include <chrono>` to header if not already present (`<charconv>` goes in the `.cpp`, NOT the header); add implicit-conversion constructors for all 13 new types to `PolicyRule`; add `static_assert(std::is_copy_constructible_v<PolicyRule>)` after the `PolicyRule` definition to verify FR-019

**Checkpoint**: Header compiles. Existing tests still pass. New variant types are constructible but `RuleEngine::evaluate` falls through to `return false` for all new types (all new branches absent from `.cpp`).

---

## Phase 3: User Story 1 — Tag Substring and Membership Matching (Priority: P1) 🎯 MVP

**Goal**: `TagContains`, `TagStartsWith`, `TagIn`, `TagExists` are fully evaluated by the rule engine.

**Independent Test**: Configure a `PolicyStageConfig` with one rule per new node type; submit events that match and events that don't; assert `Verdict::Block` / `Verdict::Pass` as appropriate. Run `ctest -R fre_unit_policy_leaf_nodes`.

### Tests for User Story 1

> **Write these tests FIRST and confirm they FAIL before implementing.**

- [x] T00X [US1] Write SCENARIO blocks for `TagContains` (match, no-match, absent tag, empty substring), `TagStartsWith` (match, no-match, absent tag, empty prefix), `TagIn` (match, no-match, absent tag, empty set), and `TagExists` (present tag, absent tag) in `tests/unit/test_policy_leaf_nodes.cpp`; also add one SCENARIO that wraps a new leaf in `And{TagContains{...}, TagExists{...}}` and one in `Not{TagContains{...}}` to verify FR-018 composability at unit level — confirm all tests FAIL (fall-through `return false` yields wrong verdict)

### Implementation for User Story 1

- [x] T00X [US1] Implement `TagContains`, `TagStartsWith`, `TagIn`, `TagExists` branches in the `std::visit` lambda in `src/policy/rule_engine.cpp` — confirm T005 tests pass

**Checkpoint**: `ctest -R fre_unit_policy_leaf_nodes` passes for all US1 scenarios.

---

## Phase 4: User Story 2 — Numeric Tag Value Comparisons (Priority: P1)

**Goal**: `TagValueLessThan`, `TagValueGreaterThan`, `TagValueBetween` correctly parse and compare numeric tag values using `std::from_chars`.

**Independent Test**: Submit events with numeric tag values above, below, and at each boundary; verify correct verdicts. Also verify absent-tag and non-numeric-value cases return false. Run `ctest -R fre_unit_policy_leaf_nodes`.

### Tests for User Story 2

> **Write these tests FIRST and confirm they FAIL before implementing.**

- [x] T00X [US2] Write SCENARIO blocks for `TagValueGreaterThan` (above threshold, below threshold), `TagValueLessThan` (below threshold, above threshold), `TagValueBetween` (`[lo, hi)` inclusive lower, exclusive upper, out-of-range, `lo >= hi`), and edge cases (absent tag, non-numeric value e.g. `"abc"`) in `tests/unit/test_policy_leaf_nodes.cpp` — confirm tests FAIL

### Implementation for User Story 2

- [x] T00X [US2] Implement `TagValueLessThan`, `TagValueGreaterThan`, `TagValueBetween` branches in `src/policy/rule_engine.cpp` using `std::from_chars` for non-throwing `double` parsing; add `#include <charconv>` to `rule_engine.cpp` (not the header); return `false` on absent tag or parse failure — confirm T007 tests pass

**Checkpoint**: `ctest -R fre_unit_policy_leaf_nodes` passes for all US1 + US2 scenarios.

---

## Phase 5: User Story 3 — First-Class Event Field Matching (Priority: P2)

**Goal**: `EventTypeIs`, `EventTypeIn`, `TenantIs`, `EventOlderThan`, `EventNewerThan` match directly against `Event` struct fields without requiring tag encoding.

**Independent Test**: Submit events with known `event_type`, `tenant_id`, and controlled `timestamp` values; verify match/no-match. Test future-timestamped events return false for both age predicates. Run `ctest -R fre_unit_policy_leaf_nodes`.

### Tests for User Story 3

> **Write these tests FIRST and confirm they FAIL before implementing.**

- [x] T00X [US3] Write SCENARIO blocks for `EventTypeIs` (match, no-match), `EventTypeIn` (member, non-member, empty set), `TenantIs` (match, no-match), `EventOlderThan` (old event, recent event, future-timestamped event, zero duration), `EventNewerThan` (recent event, old event, future-timestamped event, zero duration) in `tests/unit/test_policy_leaf_nodes.cpp` — confirm tests FAIL

### Implementation for User Story 3

- [x] T01X [US3] Implement `EventTypeIs`, `EventTypeIn`, `TenantIs`, `EventOlderThan`, `EventNewerThan` branches in `src/policy/rule_engine.cpp`; use `std::chrono::system_clock::now()` at evaluation time for age predicates; guard against negative age (future timestamp → false) — confirm T009 tests pass

**Checkpoint**: `ctest -R fre_unit_policy_leaf_nodes` passes for all US1–US3 scenarios.

---

## Phase 6: User Story 4 — Evaluator Score Range and Pipeline Health Predicates (Priority: P2)

**Goal**: `EvaluatorScoreBetween`, `StageIsDegraded`, `EvaluatorWasSkipped`, `EvaluatorReasonIs` inspect `PolicyContext::stage_outputs` to match evaluator scores and pipeline health state.

**Independent Test**: Use stub evaluators that return controlled `score`, `skipped`, `reason_code`, and `degraded_reason` values; verify each new node matches when expected and returns false otherwise (including absent evaluator/stage). Run `ctest -R fre_unit_policy_leaf_nodes`.

### Tests for User Story 4

> **Write these tests FIRST and confirm they FAIL before implementing.**

- [x] T01X [US4] Write SCENARIO blocks for `EvaluatorScoreBetween` (in-range `[lo, hi)`, below lo, at lo boundary, above hi, `lo >= hi`, absent evaluator), `StageIsDegraded` (degraded stage, clean stage, absent stage), `EvaluatorWasSkipped` (skipped=true, skipped=false, absent evaluator), `EvaluatorReasonIs` (matching reason_code, wrong reason_code, absent reason_code, absent evaluator) in `tests/unit/test_policy_leaf_nodes.cpp` — confirm tests FAIL

### Implementation for User Story 4

- [x] T01X [US4] Implement `EvaluatorScoreBetween`, `StageIsDegraded`, `EvaluatorWasSkipped`, `EvaluatorReasonIs` branches in `src/policy/rule_engine.cpp`; use `fre::is_degraded()` for the degraded check; guard `score.has_value()` and `reason_code.has_value()` before dereference — confirm T011 tests pass

**Checkpoint**: `ctest -R fre_unit_policy_leaf_nodes` passes for all US1–US4 scenarios. All 13 new node types covered.

---

## Phase 7: Integration and Polish

**Purpose**: End-to-end pipeline test with composite rules, sanitizer runs, and changelog.

- [x] T01X Write a full pipeline integration test using `fre::testing::PipelineTestHarness` with a `PolicyStageConfig` rule combining new leaf nodes inside `And`/`Or`/`Not` (e.g. `And{TagContains{"ua","bot"}, Not{StageIsDegraded{"eval"}}}`) in `tests/integration/test_policy_leaf_node_integration.cpp`; run `ctest -R fre_integration_policy_leaf_nodes` and confirm correct end-to-end `Decision::final_verdict`
- [x] T01X [P] Run sanitizer suite and confirm all pass: `for preset in asan ubsan tsan; do cmake --preset $preset && cmake --build --preset $preset && ctest --preset $preset --output-on-failure; done`
- [x] T01X [P] Update `CLAUDE.md` Recent Changes section: add entry for `005-policy-leaf-nodes` describing the 13 new leaf node types and changed files

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: No dependencies — start immediately; T002 and T003 are [P]
- **Phase 2 (Foundational)**: Depends on Phase 1 — T004 BLOCKS all user story phases
- **Phase 3 (US1)**: Depends on T004; T005 (test) must complete and FAIL before T006 (impl)
- **Phase 4 (US2)**: Depends on T004; T007 must FAIL before T008; can start after T004 regardless of US1 status
- **Phase 5 (US3)**: Depends on T004; T009 must FAIL before T010
- **Phase 6 (US4)**: Depends on T004; T011 must FAIL before T012
- **Phase 7 (Polish)**: T013 depends on T006+T008+T010+T012 all passing; T014+T015 are [P]

### User Story Dependencies

- **US1 (P1)**: Unblocked after T004
- **US2 (P1)**: Unblocked after T004; independent of US1 (different `if constexpr` branches)
- **US3 (P2)**: Unblocked after T004; independent of US1/US2
- **US4 (P2)**: Unblocked after T004; independent of US1/US2/US3

### Within Each User Story

1. Write test → confirm FAIL (red)
2. Implement → confirm PASS (green)
3. Do not proceed to next story until current story's tests are green

### Within Each Source File

- `include/fre/policy/rule_engine.hpp`: T004 only — one focused change, all new declarations
- `src/policy/rule_engine.cpp`: T006 → T008 → T010 → T012 (sequential, same file)
- `tests/unit/test_policy_leaf_nodes.cpp`: T005 → T007 → T009 → T011 (sequential, same file)
- `tests/integration/test_policy_leaf_node_integration.cpp`: T013 only

---

## Parallel Opportunities

### Phase 1
```
T002: Register unit test in tests/unit/CMakeLists.txt
T003: Register integration test in tests/integration/CMakeLists.txt
(both [P] — different files)
```

### Phase 7
```
T014: Run sanitizer suite
T015: Update CLAUDE.md
(both [P] — independent)
```

### Across User Stories (if split across developers)
```
# After T004 completes, US1 and US2 tests can be drafted in parallel (different SCENARIO sections):
T005: Write US1 test sections in test_policy_leaf_nodes.cpp
T007: Write US2 test sections in test_policy_leaf_nodes.cpp
# However both target the same file — coordinate to avoid merge conflicts
```

---

## Implementation Strategy

### MVP (User Story 1 only — 4 node types)

1. Complete Phase 1: Setup (T001–T003)
2. Complete Phase 2: Foundational (T004)
3. Complete Phase 3: US1 (T005–T006)
4. **STOP and VALIDATE**: `ctest -R fre_unit_policy_leaf_nodes` green for US1

### Incremental Delivery

1. Setup + Foundational → 13 types declared, zero implementations
2. US1 complete → `TagContains`, `TagStartsWith`, `TagIn`, `TagExists` working
3. US2 complete → numeric tag comparisons working
4. US3 complete → event field predicates working
5. US4 complete → score range and health predicates working
6. Integration + polish → full suite + sanitizers green

---

## Notes

- TDD is non-negotiable (constitution Principle II): each test task MUST produce failing tests before the corresponding implementation task runs
- `RuleEngine::evaluate` must never throw for any new branch — enforce with `noexcept` audit if desired
- `std::from_chars` requires `#include <charconv>` — add to `rule_engine.cpp` in T008 (not the header; structs in the header don't use charconv types)
- `TagIn` / `EventTypeIn` use `std::vector<std::string>` — no new deps; `<vector>` already transitively included
- `EventOlderThan` / `EventNewerThan` duration fields: use `std::chrono::milliseconds` for consistency with rest of codebase
- Commit after each user story phase (T006, T008, T010, T012) with message format `feat: add <US description> leaf nodes`
