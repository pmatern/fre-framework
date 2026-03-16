# Feature Specification: Synchronous Blocking Submit API

**Feature Branch**: `002-sync-submit-api`
**Created**: 2026-03-15
**Status**: Draft
**Input**: User description: "add a synchronous blocking API to the pipeline that allows a caller to submit a single event and block until the decision is returned, as an alternative to the existing fire-and-forget async submit. The blocking call should respect the pipeline latency budget as a timeout and return the Decision directly (or an error if the pipeline is draining, the rate limit is exceeded, or the timeout expires)."

## Clarifications

### Session 2026-03-15

- Q: For a *successful* blocking submit, should registered emission targets also be called? → A: Yes — emission targets fire on success, same as async; caller also receives the Decision directly.
- Q: Should the blocking submit support caller-initiated cancellation before timeout? → A: Yes — via a flag passed to the call; the call unblocks and returns a cancellation error when the flag is set.
- Q: Should blocking submits produce the same structured log entries and pipeline metrics as async submits? → A: Yes — identical observability to async submits.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Blocking Submit Returns Decision (Priority: P1)

A library consumer submits a single event and receives a `Decision` back in the same call, without needing to set up a callback, polling loop, or emission target to capture the result. This covers the most common integration pattern: request-in, decision-out, in-process. On success, registered emission targets also fire exactly as they would for an async submit.

**Why this priority**: Eliminates the largest integration burden for synchronous callers. The fire-and-forget API requires wiring up an emission target and a synchronization mechanism just to get a single answer; the blocking API removes that entirely and delivers immediate value.

**Independent Test**: Can be fully tested by submitting one event through a running pipeline and asserting that the returned `Decision` matches the expected verdict — no emission target needed.

**Acceptance Scenarios**:

1. **Given** a running pipeline with at least one evaluator, **When** a caller invokes the blocking submit with a valid event, **Then** the call returns a `Decision` containing the final verdict before the pipeline's latency budget elapses.
2. **Given** a pipeline whose latency budget is 300 ms, **When** the blocking submit is invoked and all stages complete within budget, **Then** the call returns with a decision and the elapsed wall-clock time is at most 300 ms.
3. **Given** a pipeline configured with multiple evaluators and a registered emission target, **When** a blocking submit succeeds, **Then** the returned decision is identical in content to what is delivered to the emission target for the same event.

---

### User Story 2 - Timeout and Error Propagation (Priority: P2)

A caller that uses the blocking API receives a typed error — not a silent failure or a hang — when the pipeline cannot honour the request within budget, is rate-limited, or is shutting down. The caller can distinguish between different failure modes and react appropriately.

**Why this priority**: Without clear error propagation the blocking API is unsafe: callers cannot tell apart "pipeline busy" from "pipeline broken" from "timed out". This story makes the API production-usable.

**Independent Test**: Can be fully tested by inducing each failure condition (timeout, rate-limit exhaustion, draining) independently and asserting the correct error variant is returned within the expected time bound.

**Acceptance Scenarios**:

1. **Given** a pipeline whose stages collectively exceed the latency budget, **When** the blocking submit is invoked, **Then** the call returns a timeout error variant within approximately the latency budget (no indefinite hang).
2. **Given** a pipeline that has exhausted its concurrent-request allowance, **When** an additional blocking submit is attempted, **Then** the call returns a rate-limit error immediately (does not queue indefinitely).
3. **Given** a pipeline that is in the draining or stopped state, **When** a blocking submit is attempted, **Then** the call returns a pipeline-unavailable error immediately.
4. **Given** any error condition, **When** the blocking submit returns an error, **Then** no decision is emitted to registered emission targets for that event (the caller holds the only result path).

---

### User Story 3 - Coexistence with Async Submit (Priority: P3)

Both the blocking and async `submit` calls operate on the same running pipeline simultaneously without interfering with each other. Mixed callers — some firing-and-forgetting, others blocking — share the same pipeline instance and rate-limit pool.

**Why this priority**: Many embedding scenarios mix batch/background processing (async) with synchronous request-handling paths. Both modes must coexist safely.

**Independent Test**: Can be fully tested by submitting a mix of async and blocking events to the same pipeline instance and asserting all decisions are correctly produced with no cross-contamination.

**Acceptance Scenarios**:

1. **Given** a pipeline receiving concurrent async and blocking submits, **When** both reach the evaluation stage simultaneously, **Then** each call returns (or emits) the correct decision for its own event.
2. **Given** a shared rate-limit pool, **When** async and blocking calls are issued concurrently, **Then** rate limiting applies uniformly — neither mode bypasses or starves the other.

---

### Edge Cases

- What happens when the event is invalid (missing required fields)? The blocking call must return a validation error, same as the async path, not hang.
- What happens if the pipeline's latency budget is set to zero or near-zero? The call should return a timeout error immediately rather than producing undefined behaviour.
- What happens if the pipeline is destroyed while a blocking call is in-flight? The call must unblock and return an error; no resource leak.
- What happens if the cancellation flag is set before the call is made? The call must return a `Cancelled` error immediately without submitting the event.
- What happens if the cancellation flag is set after the decision has already been produced? The decision is returned normally; the flag is ignored (no-op race).
- What happens when a blocking call is made on a pipeline that has not yet been started? Return a `NotStarted` error immediately.
- What happens if many threads all invoke blocking submit simultaneously under a low `max_concurrent` setting? Excess calls return rate-limit errors promptly; accepted calls eventually return decisions.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The pipeline MUST expose a blocking submit operation that accepts a single event and returns either a `Decision` or a typed error.
- **FR-002**: The blocking submit MUST honour the pipeline's configured latency budget as a maximum wall-clock timeout; it MUST NOT block longer than this budget under any circumstance.
- **FR-003**: The blocking submit MUST return a rate-limit error immediately when the pipeline's concurrent-request allowance is exhausted, consistent with the behaviour of the async submit.
- **FR-004**: The blocking submit MUST return a `PipelineUnavailable` error when the pipeline is in the draining or stopped state.
- **FR-005**: The blocking submit MUST return a `Timeout` error when the decision is not produced within the latency budget.
- **FR-006**: When a blocking submit returns an error, the pipeline MUST NOT emit a decision for that event to any registered emission targets.
- **FR-007**: When a blocking submit succeeds, the pipeline MUST deliver the decision to all registered emission targets in addition to returning it to the caller.
- **FR-008**: The blocking submit MUST be safe to call concurrently from multiple threads on the same pipeline instance.
- **FR-009**: The async `submit` and blocking submit MUST be independently usable on the same pipeline instance without interference.
- **FR-010**: Invoking the blocking submit on a pipeline that has not been started MUST return a `NotStarted` error immediately.
- **FR-011**: The `Decision` returned by a blocking submit MUST be identical in content to the decision delivered to emission targets for the same event.
- **FR-012**: The blocking submit MUST accept a caller-supplied cancellation flag; when the flag is set while the call is in-flight, the call MUST unblock and return a `Cancelled` error without emitting a decision.
- **FR-013**: If the cancellation flag is set before the call is made, the call MUST return `Cancelled` immediately without submitting the event to the pipeline.
- **FR-014**: If the cancellation flag is set after the decision has already been produced, the decision MUST be returned to the caller normally (cancellation is a no-op).
- **FR-015**: The pipeline MUST emit the same structured log entries for a blocking submit as it does for an async submit (event received, stage transitions, decision produced or error).
- **FR-016**: The pipeline MUST record the same pipeline metrics for a blocking submit as for an async submit (event count, decision count, error count, latency).

### Key Entities

- **BlockingSubmitResult**: The outcome of a blocking submit — either a `Decision` (success) or a typed error (timeout, rate-limited, pipeline-unavailable, validation-failed, not-started, cancelled).
- **SubmitSyncError**: An enumeration of distinct failure reasons: `Timeout`, `RateLimited`, `PipelineUnavailable`, `ValidationFailed`, `NotStarted`, `Cancelled`.
- **CancellationFlag**: A caller-supplied flag passed to the blocking submit call; when set, causes the in-flight call to unblock and return `Cancelled`.
- **Decision**: The existing pipeline output structure — verdict, stage outputs, evaluator results — unchanged.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A blocking submit on an otherwise-idle pipeline completes and returns a decision in under the pipeline's configured latency budget in 99% of calls.
- **SC-002**: A blocking submit under timeout conditions returns an error within 110% of the latency budget (no more than 10% overrun from internal overhead).
- **SC-003**: Pipelines under mixed async and blocking load of 1 000 concurrent callers sustain zero cross-contamination of decisions (each caller receives only its own decision).
- **SC-004**: All six error variants (Timeout, RateLimited, PipelineUnavailable, ValidationFailed, NotStarted, Cancelled) are distinguishable by callers with no ambiguity.
- **SC-006**: When a cancellation flag is set while a blocking call is in-flight, the call unblocks within one scheduling quantum (no busy-wait or indefinite delay).
- **SC-007**: Structured log entries for blocking submits are indistinguishable in format and completeness from those produced by async submits for the same event and decision.
- **SC-005**: Existing async submit tests continue to pass without modification after the blocking API is introduced (no regression).

## Assumptions

- The latency budget configured in `PipelineConfig` serves as the natural timeout for blocking submits; no separate per-call timeout parameter is needed at the initial scope.
- On a successful blocking submit, emission targets fire in addition to the caller receiving the Decision directly; this is consistent with async behaviour and supports audit/logging pipelines.
- Thread safety is required: the blocking API must be callable from any thread without external synchronisation by the caller.
- The blocking API is in-process only; no network or IPC boundary is introduced by this feature.
- The `Decision` structure is not modified; only the delivery mechanism changes.
