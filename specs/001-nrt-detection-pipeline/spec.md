# Feature Specification: Near Real-Time Detection Pipeline Framework

**Feature Branch**: `001-nrt-detection-pipeline`
**Created**: 2026-03-15
**Status**: Draft
**Input**: User description: "create the underlying framework used to create near real time
monitoring/detection/evaluation/decisioning components. The various stages of 1. event ingest,
2. lightweight evaluation like allow/deny lists and thresholds on windowed aggregation,
3. ml inference including anomaly detection on behavioral or structural embedding,
4. lightweight policy evaluation, 5. decision emission should all be configurable and pluggable."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Define and Run a Detection Pipeline (Priority: P1)

A platform engineer needs to assemble a detection pipeline from individually configured stages.
They declare which stages are active, supply configuration for each stage (evaluators, thresholds,
policies), and start the pipeline against a stream of incoming events. The pipeline processes each
event through the active stages in order and emits a structured decision for every event that
reaches the emission stage.

**Why this priority**: This is the core value proposition of the framework. Without the ability to
assemble and run a pipeline end-to-end, no other capability is meaningful.

**Independent Test**: A minimal pipeline with ingest → lightweight evaluation → decision emission
can be assembled, started, fed a set of synthetic events, and produce verifiable decisions — all
without any ML inference or policy evaluation stages present.

**Acceptance Scenarios**:

1. **Given** a pipeline definition with at least event ingest and decision emission configured,
   **When** an event is submitted to the pipeline,
   **Then** a decision record is emitted that references the originating event and includes the
   outcome determined by each active stage.

2. **Given** a pipeline definition missing a required stage configuration,
   **When** the pipeline is started,
   **Then** startup fails with a clear error identifying the missing or misconfigured stage before
   any events are processed.

3. **Given** a running pipeline,
   **When** a new event arrives,
   **Then** the end-to-end latency from event receipt to decision emission is within the configured
   latency budget (default: 300ms P99 as observed by the caller).

---

### User Story 2 - Plug In a Custom Stage Evaluator (Priority: P1)

A platform engineer has domain-specific evaluation logic (e.g., a proprietary allow/deny list
source, a custom threshold formula, or a specialised ML model) that does not exist in the
framework's built-in set. They implement the evaluator against a published contract and register
it with the pipeline configuration. The custom evaluator participates in the pipeline exactly as
a built-in evaluator would.

**Why this priority**: Pluggability is the framework's primary extension mechanism. If third-party
evaluators cannot integrate cleanly, teams will fork the framework, defeating its purpose.

**Independent Test**: A synthetic custom evaluator implementing the evaluator contract can be
registered for any stage, and events processed through that stage invoke the custom evaluator and
incorporate its output into the decision.

**Acceptance Scenarios**:

1. **Given** a custom evaluator registered for the lightweight evaluation stage,
   **When** an event passes through that stage,
   **Then** the custom evaluator's verdict (pass/flag/block) is recorded in the stage output and
   influences the downstream stages and final decision.

2. **Given** a custom evaluator that throws an unhandled error during evaluation,
   **When** the error occurs,
   **Then** the pipeline applies the configured failure mode for that stage (fail-open, fail-closed,
   or emit a degraded decision) without crashing other concurrent evaluations.

3. **Given** two custom evaluators registered for the same stage,
   **When** an event is evaluated,
   **Then** both evaluators run and their verdicts are combined according to the stage's configured
   composition rule (AnyBlock — block if any blocks; AnyFlag — flag if any flags; Unanimous —
   block only if all block; Majority — block if more than half block).

---

### User Story 3 - Configure Windowed Aggregation Thresholds (Priority: P2)

An operator needs to detect anomalous event rates or volumes from a single entity (e.g., a user,
device, or tenant) over a rolling time window. They configure a window size, an aggregation
function (count, sum, distinct-count), and a threshold value. When the aggregate crosses the
threshold, the lightweight evaluation stage flags the event.

**Why this priority**: Windowed aggregation thresholds are the primary mechanism for catching
volumetric abuse patterns without ML. They are a foundational building block that enables the
framework to provide value before any ML stage is configured.

**Independent Test**: A pipeline with windowed aggregation configured can be fed a burst of events
from a single entity and produce flagged decisions once the configured threshold is crossed, with
events from other entities remaining unaffected.

**Acceptance Scenarios**:

1. **Given** a window of 60 seconds and a count threshold of 100 events per entity,
   **When** an entity submits 101 events within a 60-second window,
   **Then** the 101st event (and subsequent events within that window) is flagged by the
   lightweight evaluation stage.

2. **Given** two entities both approaching the threshold simultaneously,
   **When** one entity crosses the threshold,
   **Then** only that entity's events are flagged; the other entity's events continue to be
   evaluated independently.

3. **Given** a configured window that expires,
   **When** the window closes and the entity's event count resets,
   **Then** subsequent events from that entity start a fresh count and are not flagged until the
   threshold is crossed again in the new window.

---

### User Story 4 - Attach an ML Inference Evaluator (Priority: P2)

A data scientist has trained an anomaly detection model that operates on behavioural or structural
embeddings derived from events. They register this model as a pluggable ML inference evaluator
for the ML inference stage. The framework invokes the model per event (or per batch), receives a
scored result, and incorporates the score into the stage output.

**Why this priority**: ML inference is a key differentiator of the framework's detection
capability, but it is optional — pipelines without ML are fully viable, so this is P2.

**Independent Test**: A pipeline with the ML inference stage configured can process events,
invoke a registered stub inference evaluator, and record inference scores in the decision output.

**Acceptance Scenarios**:

1. **Given** an ML inference evaluator registered that returns an anomaly score between 0.0 and 1.0,
   **When** an event is processed through the ML inference stage,
   **Then** the returned score is recorded in the stage output and compared against the configured
   score threshold to produce a pass/flag verdict.

2. **Given** an ML inference stage configured with a timeout,
   **When** the inference evaluator does not respond within the timeout,
   **Then** the stage applies its configured failure mode and records a timeout event in the
   decision audit trail without blocking other stages or concurrent events.

---

### User Story 5 - Evaluate Policy Rules Against Stage Outputs (Priority: P3)

A security engineer needs to express cross-stage policy logic — for example, "block if lightweight
evaluation flagged AND ML score exceeds 0.8, but allow if entity is on the trusted list". They
author policy rules in the framework's policy language and attach them to the policy evaluation
stage. The stage evaluates rules against the accumulated stage outputs for each event.

**Why this priority**: Policy evaluation adds expressive conditional logic on top of individual
stage verdicts. It is valuable but requires earlier stages to be functional first.

**Independent Test**: A pipeline with two preceding stages and a policy evaluation stage can apply
a composite rule referencing both stage outputs and produce a correct allow/block verdict.

**Acceptance Scenarios**:

1. **Given** a policy rule that references outputs from both the lightweight evaluation and ML
   inference stages,
   **When** an event produces stage outputs that satisfy the rule's conditions,
   **Then** the policy evaluation stage emits a block verdict that is carried into the final
   decision.

2. **Given** a policy rule referencing a stage that was not active in the pipeline,
   **When** the pipeline is started,
   **Then** startup fails with an error identifying the missing stage dependency.

---

### Edge Cases

- What happens when an event arrives with a malformed or missing entity identifier required for
  windowed aggregation?
- How does the pipeline behave if the decision emission target is unavailable or slow?
- What happens when two windowed aggregation rules share the same window key but different
  aggregation functions?
- How are events handled that arrive after their associated window has already closed (late arrivals)?
- What is the behaviour when all active evaluators in a stage are degraded simultaneously?

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The framework MUST provide a pipeline abstraction that composes an ordered sequence
  of named stages: event ingest, lightweight evaluation, ML inference, policy evaluation, and
  decision emission.
- **FR-002**: Each stage MUST be independently optional and omittable from a pipeline definition
  without requiring changes to other stages.
- **FR-003**: Each stage MUST expose a pluggable evaluator contract that third-party implementations
  can satisfy without modifying framework internals.
- **FR-004**: The lightweight evaluation stage MUST support allow/deny list evaluators that accept
  configurable list sources and match criteria.
- **FR-005**: The lightweight evaluation stage MUST support windowed aggregation evaluators
  configurable with: window duration, aggregation function (count, sum, distinct-count), grouping
  key, and numeric threshold.
- **FR-006**: The ML inference stage MUST support pluggable inference evaluators that accept an
  event (or batch) and return a numeric score; the framework manages the invocation lifecycle and
  timeout enforcement.
- **FR-007**: The policy evaluation stage MUST support declarative rules that compose verdicts from
  earlier stage outputs using logical operators (AND, OR, NOT).
- **FR-008**: The decision emission stage MUST support pluggable emission targets; each target
  receives a structured decision record containing: event reference, per-stage verdicts, final
  outcome, and a processing timestamp.
- **FR-009**: The framework MUST enforce the latency budget (P99 ≤ 300ms from event receipt to
  decision emission) by applying configurable per-stage timeouts that sum to less than the budget.
- **FR-010**: The framework MUST apply per-tenant isolation such that a processing failure or
  resource exhaustion in one tenant's event stream does not degrade evaluation for other tenants.
- **FR-011**: The framework MUST support configurable failure modes per stage (fail-open,
  fail-closed, emit-degraded-decision) that activate when an evaluator exceeds its timeout or
  raises an unhandled error.
- **FR-012**: The framework MUST produce a structured audit trail per event recording: which stages
  ran, which evaluators were invoked, each verdict, any degraded or timeout states, and total
  elapsed time.
- **FR-013**: Pipeline configuration MUST be validated at startup; any missing required
  configuration or invalid stage dependency MUST produce an actionable error before the pipeline
  accepts events.
- **FR-014**: The framework MUST be usable as an embeddable library (running in-process within a
  host application) as its primary deployment model. An optional thin service harness MUST wrap
  the library for teams that prefer a standalone deployable runtime with its own event ingestion
  API and health-check surface.
- **FR-015**: Windowed aggregation state MUST default to in-process storage per pipeline instance.
  The framework MUST additionally support an externalized state backend via a pluggable state-store
  contract, enabling shared windowed state across multiple pipeline instances when required.

- **FR-016**: The framework's test suite MUST be runnable under AddressSanitizer (ASan),
  UndefinedBehaviorSanitizer (UBSan), and ThreadSanitizer (TSan) with zero reported errors; all
  three sanitizer configurations MUST be required CI quality gates that block merge on failure.
- **FR-017**: The test suite MUST achieve a minimum of 80% line coverage as measured by a
  coverage instrumentation tool; the coverage gate MUST be enforced in CI and block merge when
  the threshold is not met.
- **FR-018**: All framework source code MUST pass clang-tidy static analysis with warnings
  treated as errors; a `.clang-tidy` configuration file MUST be present at the repository root
  and MUST document any approved check suppressions with justification comments.
- **FR-019**: Valgrind memcheck MUST be executed against the full test suite on a Linux CI job
  as a non-blocking advisory gate; results MUST be reported and surfaced in CI output but do not
  block merge.

### Key Entities

- **Pipeline**: A named, versioned configuration of ordered active stages and their evaluator
  registrations. Has a lifecycle: stopped → starting → running → draining → stopped.
- **Stage**: A named processing unit within a pipeline. Holds registered evaluators and a verdict
  composition rule. Produces a `StageOutput` containing one or more evaluator verdicts.
- **Event**: The unit of work flowing through the pipeline. Carries an entity identifier, a
  timestamp, a payload, and optional metadata tags.
- **Evaluator**: A pluggable unit of evaluation logic bound to a specific stage contract. Returns
  a verdict (pass / flag / block) and optional metadata.
- **Window**: A time-bounded accumulator keyed by entity identifier and window configuration.
  Holds an aggregated value used for threshold comparison.
- **PolicyRule**: A declarative expression that composes `StageOutput` values from named stages
  into a final pass/block verdict using logical operators.
- **Decision**: The output record emitted for each event. Contains the event reference, per-stage
  outputs, final verdict, degradation indicators, and end-to-end latency.
- **EmissionTarget**: A pluggable sink that receives `Decision` records from the decision emission
  stage.

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A platform engineer can assemble and start a complete 5-stage pipeline using only
  configuration and registered evaluators, without writing any framework-internal code.
- **SC-002**: A new custom evaluator satisfying the published stage contract can be integrated
  into a pipeline definition without modifying any existing framework or pipeline code.
- **SC-003**: P99 end-to-end latency from event receipt to decision emission remains at or below
  300ms under sustained load representative of expected production throughput.
- **SC-004**: A burst of events from one tenant exhausting that tenant's windowed aggregation
  budget does not increase latency or error rate for any other tenant's concurrent event stream.
- **SC-005**: When any single evaluator exceeds its configured timeout, the affected event
  receives a degraded decision within the latency budget; no other concurrent events are delayed.
- **SC-006**: 100% of processed events produce a decision record with a complete audit trail,
  including degraded and failure cases.
- **SC-007**: Pipeline configuration errors are reported at startup with sufficient detail for an
  operator to resolve the issue without consulting source code.
- **SC-008**: A data scientist can register an ML inference evaluator and validate its integration
  end-to-end using the framework's built-in test harness without deploying a full production
  pipeline.

---

## Clarifications

### Session 2026-03-15

- Q: Which sanitizer builds should be required CI quality gates? → A: ASan + UBSan + TSan (memory errors, undefined behavior, data races)
- Q: What minimum line coverage percentage should be required? → A: 80% line coverage minimum
- Q: Which static analysis tools should be required CI quality gates? → A: clang-tidy only
- Q: How should Valgrind memcheck fit into quality requirements? → A: Required on Linux CI as a non-blocking advisory gate
- Q: How should clang-tidy warnings be treated? → A: Warnings-as-errors with a `.clang-tidy` suppressions file for known exceptions

- **SC-009**: All tests pass clean under ASan, UBSan, and TSan builds with zero sanitizer-reported
  errors or warnings.
- **SC-010**: CI-measured line coverage meets or exceeds 80% across the framework library; the
  coverage gate blocks merge when the threshold is not met.
- **SC-011**: clang-tidy reports zero warnings on all framework source files under the project's
  `.clang-tidy` configuration; any suppressions are documented in the config file.
- **SC-012**: Valgrind memcheck reports no definite memory errors on the Linux CI advisory job;
  results are visible in CI output for every merge.

---

## Assumptions

- The framework serves multiple tenants simultaneously; per-tenant isolation is a first-class
  concern aligned with constitution Principle VI.
- Windowed aggregation state defaults to in-process per pipeline instance. When the externalized
  state backend (FR-015) is configured, multiple instances share window state via the pluggable
  store; the framework does not prescribe which backing store is used.
- ML inference evaluators are synchronous within their timeout budget; the framework does not
  manage model loading, versioning, or serving infrastructure.
- Decision emission is asynchronous (fire-and-forward) to decouple emission latency from the
  core evaluation path; back-pressure handling is a pipeline-level configuration concern.
- Events are assumed to arrive approximately in order; configurable late-arrival handling within
  a window is in scope, but global out-of-order delivery guarantees are out of scope for v1.
- Policy rules reference stage outputs by stage name; rules referencing absent stages are invalid
  and prevent pipeline startup.
