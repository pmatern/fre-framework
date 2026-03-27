# Feature Specification: Extended Policy Rule Leaf Nodes

**Feature Branch**: `005-policy-leaf-nodes`
**Created**: 2026-03-26
**Status**: Draft
**Input**: User description: "add everything you suggested which doesn't require a regex. be sure to add the appropriate tests."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Tag substring and membership matching (Priority: P1)

A pipeline author wants to match events where a tag value contains a substring, starts with a prefix,
or belongs to a set of known values — without constructing a chain of `Or{TagEquals, TagEquals, ...}` nodes.

**Why this priority**: Tag-based matching is the most common policy predicate. Substring and set
membership are the most frequently needed extensions beyond exact match, and directly reduce
boilerplate in current rule sets.

**Independent Test**: Configure rules using `TagContains`, `TagStartsWith`, `TagIn`, and `TagExists`;
submit events that satisfy each condition and events that do not; assert correct Block/Flag/Pass verdicts.

**Acceptance Scenarios**:

1. **Given** a rule `TagContains{"user_agent", "bot"}`, **When** an event arrives with tag `user_agent=crawlerbot/1.0`, **Then** the rule matches.
2. **Given** a rule `TagContains{"user_agent", "bot"}`, **When** an event arrives with tag `user_agent=Firefox/120`, **Then** the rule does not match.
3. **Given** a rule `TagStartsWith{"country", "US"}`, **When** an event has tag `country=US-CA`, **Then** the rule matches.
4. **Given** a rule `TagStartsWith{"country", "US"}`, **When** an event has tag `country=GB`, **Then** the rule does not match.
5. **Given** a rule `TagIn{"risk_tier", {"high","critical"}}`, **When** an event has tag `risk_tier=high`, **Then** the rule matches.
6. **Given** a rule `TagIn{"risk_tier", {"high","critical"}}`, **When** an event has tag `risk_tier=low`, **Then** the rule does not match.
7. **Given** a rule `TagExists{"session_id"}`, **When** an event has that tag present (any value), **Then** the rule matches.
8. **Given** a rule `TagExists{"session_id"}`, **When** an event has no such tag, **Then** the rule does not match.

---

### User Story 2 - Numeric tag value comparisons (Priority: P1)

A pipeline author wants to compare a tag's value interpreted as a number — for example flagging
events where a request-count tag exceeds a threshold, or a score falls within a suspicious range.

**Why this priority**: Many enrichment pipelines attach numeric metadata as tags. Currently the only
numeric threshold available is `EvaluatorScoreAbove`, which requires a dedicated evaluator. Direct
tag numeric comparison removes that indirection.

**Independent Test**: Configure rules using `TagValueLessThan`, `TagValueGreaterThan`, and
`TagValueBetween`; submit events with tag values on both sides of each boundary; assert correct
verdicts. Verify graceful handling of absent or non-numeric tag values.

**Acceptance Scenarios**:

1. **Given** a rule `TagValueGreaterThan{"request_count", 1000}`, **When** tag `request_count=1500`, **Then** the rule matches.
2. **Given** a rule `TagValueGreaterThan{"request_count", 1000}`, **When** tag `request_count=500`, **Then** the rule does not match.
3. **Given** a rule `TagValueLessThan{"error_rate", 0.05}`, **When** tag `error_rate=0.01`, **Then** the rule matches.
4. **Given** a rule `TagValueBetween{"score", 0.4, 0.8}`, **When** tag `score=0.6`, **Then** the rule matches.
5. **Given** a rule `TagValueBetween{"score", 0.4, 0.8}`, **When** tag `score=0.9`, **Then** the rule does not match.
6. **Given** any numeric tag rule, **When** the tag is absent, **Then** the rule returns false with no error.
7. **Given** any numeric tag rule, **When** the tag value is not a valid number (e.g. `"abc"`), **Then** the rule returns false with no error.

---

### User Story 3 - First-class Event field matching (Priority: P2)

A pipeline author wants to write rules that target the structured fields of an event directly —
`event_type`, `tenant_id`, and event age — without needing to duplicate that information as tags.

**Why this priority**: `event_type` and `tenant_id` are on every event. Requiring them to be
redundantly encoded as tags to be matchable is unnecessary friction and a source of inconsistency.

**Independent Test**: Configure `EventTypeIs`, `EventTypeIn`, `TenantIs`, `EventOlderThan`, and
`EventNewerThan` rules; submit events with matching and non-matching field values; assert correct verdicts.

**Acceptance Scenarios**:

1. **Given** a rule `EventTypeIs{"login_attempt"}`, **When** an event has `event_type=login_attempt`, **Then** the rule matches.
2. **Given** a rule `EventTypeIs{"login_attempt"}`, **When** an event has `event_type=api_call`, **Then** the rule does not match.
3. **Given** a rule `EventTypeIn{{"login_attempt","password_reset"}}`, **When** `event_type=password_reset`, **Then** the rule matches.
4. **Given** a rule `TenantIs{"acme"}`, **When** `tenant_id=acme`, **Then** the rule matches.
5. **Given** a rule `TenantIs{"acme"}`, **When** `tenant_id=globex`, **Then** the rule does not match.
6. **Given** a rule `EventOlderThan{30s}`, **When** an event's timestamp is 60 seconds in the past, **Then** the rule matches.
7. **Given** a rule `EventOlderThan{30s}`, **When** an event's timestamp is 10 seconds in the past, **Then** the rule does not match.
8. **Given** a rule `EventNewerThan{5s}`, **When** an event's timestamp is 1 second in the past, **Then** the rule matches.
9. **Given** a rule `EventNewerThan{5s}`, **When** an event's timestamp is 10 seconds in the past, **Then** the rule does not match.

---

### User Story 4 - Evaluator score range and pipeline health predicates (Priority: P2)

A pipeline author wants to express a score band (not just a lower bound), check whether a stage ran
degraded, detect a skipped evaluator, or match on an evaluator's reason code — enabling defensive
rules that respond to pipeline health.

**Why this priority**: `EvaluatorScoreAbove` covers only a lower bound; expressing a score band today
requires verbose double-negation. Stage/evaluator health predicates enable rules like "block if
inference was skipped due to timeout."

**Independent Test**: Configure rules using `EvaluatorScoreBetween`, `StageIsDegraded`,
`EvaluatorWasSkipped`, and `EvaluatorReasonIs`; simulate degraded/skipped evaluator states via stub
evaluators; assert correct pipeline verdicts.

**Acceptance Scenarios**:

1. **Given** a rule `EvaluatorScoreBetween{"model", 0.4, 0.8}`, **When** evaluator reports `score=0.6`, **Then** the rule matches.
2. **Given** a rule `EvaluatorScoreBetween{"model", 0.4, 0.8}`, **When** evaluator reports `score=0.9`, **Then** the rule does not match.
3. **Given** a rule `EvaluatorScoreBetween{"model", 0.4, 0.8}`, **When** evaluator reports `score=0.4` (lower bound), **Then** the rule matches (inclusive lower bound).
4. **Given** a rule `StageIsDegraded{"inference"}`, **When** the inference stage ran with a non-zero degraded reason, **Then** the rule matches.
5. **Given** a rule `StageIsDegraded{"inference"}`, **When** the inference stage ran cleanly, **Then** the rule does not match.
6. **Given** a rule `EvaluatorWasSkipped{"slow_model"}`, **When** that evaluator's result has `skipped=true`, **Then** the rule matches.
7. **Given** a rule `EvaluatorReasonIs{"allow_list", "trusted_entity"}`, **When** that evaluator's `reason_code` equals `"trusted_entity"`, **Then** the rule matches.

---

### Edge Cases

- `TagValueBetween` with `lo >= hi` — rule never matches (vacuously false); no panic or undefined behaviour.
- `TagIn` with an empty value set — rule never matches.
- `EventOlderThan` / `EventNewerThan` with a zero duration — defined as non-matching.
- `EvaluatorScoreBetween` referencing an evaluator that did not run — false, no error.
- `StageIsDegraded` referencing a stage that did not run — false, no error.
- `EvaluatorWasSkipped` / `EvaluatorReasonIs` referencing an evaluator that did not run — false, no error.
- All new nodes are leaf nodes with value semantics; no `unique_ptr` children; straightforward copy.

## Requirements *(mandatory)*

### Functional Requirements

**Tag string matching**

- **FR-001**: The rule engine MUST support `TagContains{key, substring}` — matches when the named tag's value contains the substring (case-sensitive).
- **FR-002**: The rule engine MUST support `TagStartsWith{key, prefix}` — matches when the named tag's value begins with the given prefix.
- **FR-003**: The rule engine MUST support `TagIn{key, values}` — matches when the named tag's value is an exact member of the provided set.
- **FR-004**: The rule engine MUST support `TagExists{key}` — matches when the event carries the named tag regardless of its value.

**Numeric tag comparisons**

- **FR-005**: The rule engine MUST support `TagValueLessThan{key, threshold}` — parses the tag value as a double and matches when it is strictly less than threshold.
- **FR-006**: The rule engine MUST support `TagValueGreaterThan{key, threshold}` — matches when the parsed double is strictly greater than threshold.
- **FR-007**: The rule engine MUST support `TagValueBetween{key, lo, hi}` — matches when the parsed double satisfies `lo <= value < hi`.
- **FR-008**: All numeric tag rules MUST return false without error when the tag is absent or its value cannot be parsed as a number.

**Event field matching**

- **FR-009**: The rule engine MUST support `EventTypeIs{event_type}` — exact match on `Event::event_type`.
- **FR-010**: The rule engine MUST support `EventTypeIn{event_types}` — membership match on `Event::event_type`.
- **FR-011**: The rule engine MUST support `TenantIs{tenant_id}` — exact match on `Event::tenant_id`.
- **FR-012**: The rule engine MUST support `EventOlderThan{duration}` — matches when `now - event.timestamp > duration`.
- **FR-013**: The rule engine MUST support `EventNewerThan{duration}` — matches when `now - event.timestamp < duration`.

**Evaluator score range and pipeline health**

- **FR-014**: The rule engine MUST support `EvaluatorScoreBetween{evaluator_id, lo, hi}` — matches when the named evaluator's score satisfies `lo <= score < hi`.
- **FR-015**: The rule engine MUST support `StageIsDegraded{stage_id}` — matches when the named stage's `degraded_reason` is non-zero.
- **FR-016**: The rule engine MUST support `EvaluatorWasSkipped{evaluator_id}` — matches when any evaluator result with that ID has `skipped=true`.
- **FR-017**: The rule engine MUST support `EvaluatorReasonIs{evaluator_id, reason_code}` — matches when any evaluator result with that ID has the exact given `reason_code`.

**General**

- **FR-018**: All new leaf nodes MUST be composable as direct children of `And`, `Or`, and `Not` without additional wrapping.
- **FR-019**: All new leaf nodes MUST have value semantics and be safely copyable as part of a `PolicyRule` tree.
- **FR-020**: `RuleEngine::evaluate` MUST NOT throw for any new node type; absent or unparseable data MUST yield false.
- **FR-021**: All new node types MUST be usable via `PolicyStageConfig::add_rule` without changes to any other pipeline stage or the builder API.

### Key Entities

- **PolicyRule**: Extended variant — gains 13 new leaf node alternatives alongside the 6 existing node types.
- **PolicyContext**: Unchanged — already carries `event` and `stage_outputs`; all new leaves read from these fields.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: All 13 new leaf node types evaluate correctly for both match and non-match cases — verified by dedicated unit tests for each node type.
- **SC-002**: All edge cases (absent tag, non-numeric tag value, absent evaluator/stage, empty `TagIn` set, zero-duration age predicates) produce false with no exception — verified by unit tests.
- **SC-003**: At least one integration test exercises a composite rule combining new leaf nodes inside `And`/`Or`/`Not` and produces the correct end-to-end pipeline verdict.
- **SC-004**: The entire existing policy and pipeline test suite passes without modification — zero regressions.
- **SC-005**: All new node types are accessible through `PolicyStageConfig::add_rule` with no changes required outside `rule_engine.hpp` and `rule_engine.cpp`.
