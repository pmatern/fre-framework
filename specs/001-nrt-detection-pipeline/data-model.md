# Data Model: Near Real-Time Detection Pipeline Framework

**Feature**: 001-nrt-detection-pipeline
**Date**: 2026-03-15

## Core Types

### Event

The atomic unit of work entering the pipeline. Immutable once created.

| Field | Type | Description |
|-------|------|-------------|
| `id` | `uint64_t` | Monotonically increasing per-pipeline sequence number |
| `tenant_id` | `string_view` | Tenant owning this event; used for sharding and isolation |
| `entity_id` | `string_view` | Entity within the tenant (user, device, session, etc.) |
| `event_type` | `string_view` | Classifier used by evaluators to filter relevance |
| `timestamp` | `std::chrono::system_clock::time_point` | Wall-clock time of event origin |
| `payload` | `std::span<const std::byte>` | Opaque binary payload; evaluators interpret per their contract |
| `tags` | `std::span<const Tag>` | Key-value metadata pairs for routing and filtering |

**Validation rules**:
- `tenant_id` and `entity_id` MUST be non-empty
- `timestamp` MUST NOT be in the future beyond a configurable skew tolerance (default: 5s)
- Late events (timestamp older than the maximum configured window duration) are accepted but
  logged as `LateArrival` in the audit trail

---

### Verdict

The output of a single evaluator's assessment of one event.

| Value | Meaning |
|-------|---------|
| `Pass` | Evaluator found nothing of concern |
| `Flag` | Evaluator found a signal worth noting; downstream stages can escalate |
| `Block` | Evaluator determined the event should be stopped |

---

### EvaluatorResult

| Field | Type | Description |
|-------|------|-------------|
| `evaluator_id` | `string_view` | Registered name of the evaluator |
| `verdict` | `Verdict` | Pass / Flag / Block |
| `score` | `std::optional<float>` | Numeric signal strength (0.0–1.0); set by ML inference evaluators |
| `reason_code` | `std::optional<string_view>` | Short machine-readable reason string |
| `metadata` | `std::span<const Tag>` | Arbitrary evaluator-specific output |

---

### StageOutput

The combined result of all evaluators in one stage for one event.

| Field | Type | Description |
|-------|------|-------------|
| `stage_id` | `StageId` | Which stage produced this output |
| `verdict` | `Verdict` | Composed verdict (per stage composition rule) |
| `evaluator_results` | `std::span<const EvaluatorResult>` | Individual evaluator outputs |
| `elapsed_us` | `uint32_t` | Microseconds spent in this stage |
| `degraded` | `bool` | True if any evaluator timed out or errored |
| `degraded_reason` | `DegradedReason` | Bitmask of degradation causes (see below) |

**Composition rules** (configured per stage):
- `AnyBlock` — Block if any evaluator returns Block
- `AnyFlag` — Flag if any evaluator returns Flag
- `Unanimous` — Block only if all evaluators return Block
- `Majority` — Block if more than half return Block
- `WeightedScore` — Block/Flag based on weighted sum of scores (ML stage default)

---

### DegradedReason (bitmask)

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | `EvaluatorTimeout` | One or more evaluators exceeded their per-stage timeout |
| 1 | `EvaluatorError` | One or more evaluators returned an error |
| 2 | `StateStoreUnavailable` | External state store could not be reached; fell back to in-process |
| 3 | `RateLimited` | Event was processed under rate-limiting conditions |
| 4 | `PartialEvaluation` | Some evaluators were skipped due to degradation policy |

---

### Decision

The final output record emitted for every event that completes the pipeline.

| Field | Type | Description |
|-------|------|-------------|
| `event_id` | `uint64_t` | Reference to originating `Event::id` |
| `tenant_id` | `string_view` | Tenant identifier (copied from Event) |
| `pipeline_id` | `string_view` | Name of the pipeline that produced this decision |
| `final_verdict` | `Verdict` | Pass / Flag / Block — the pipeline's conclusive verdict |
| `stage_outputs` | `std::span<const StageOutput>` | Per-stage results in execution order |
| `degraded` | `bool` | True if any stage was degraded |
| `degraded_reason` | `DegradedReason` | Union of all stage degraded_reason bitmasks |
| `processing_started_at` | `time_point` | When the event entered the pipeline |
| `processing_ended_at` | `time_point` | When the decision was emitted |
| `elapsed_us` | `uint32_t` | Total end-to-end processing time in microseconds |

---

### Window

An in-process accumulator for windowed aggregation thresholds.

| Field | Type | Description |
|-------|------|-------------|
| `key` | `WindowKey` | Composite key: `(tenant_id, entity_id, rule_id)` |
| `window_type` | `WindowType` | `Tumbling` or `Sliding` |
| `duration` | `std::chrono::duration` | Length of the window |
| `aggregation` | `AggregationFn` | `Count`, `Sum`, `DistinctCount` |
| `current_value` | `double` | Running aggregate value |
| `window_start` | `time_point` | Start of the current window epoch |
| `event_count` | `uint64_t` | Number of events in current window (for audit) |

**State storage**: Windows live in a lock-free time-wheel indexed by expiry slot. Each tenant's
windows are confined to that tenant's assigned shards (shuffle sharding).

---

### PolicyRule

A declarative expression evaluated against the StageOutputs for one event.

| Field | Type | Description |
|-------|------|-------------|
| `rule_id` | `string_view` | Unique rule identifier |
| `expression` | `RuleExpression` | AST of AND/OR/NOT nodes over stage verdict predicates |
| `action` | `Verdict` | Verdict to emit when expression is true |
| `priority` | `uint32_t` | Evaluation order; lower number = higher priority |

**Expression nodes**:
- `StageVerdictIs(stage_id, verdict)` — tests a stage's composed verdict
- `EvaluatorScoreAbove(evaluator_id, threshold)` — tests a numeric score
- `TagEquals(key, value)` — tests an event tag
- `And(left, right)`, `Or(left, right)`, `Not(expr)` — logical combinators

---

### PipelineConfig

Top-level configuration for one pipeline instance.

| Field | Type | Description |
|-------|------|-------------|
| `pipeline_id` | `string_view` | Unique name |
| `latency_budget_ms` | `uint32_t` | Total P99 budget (default: 300) |
| `stages` | ordered list of `StageConfig` | Active stages in execution order |
| `sharding` | `ShardingConfig` | N (cells), K (per-tenant assignment), hash function |
| `tenant_rate_limits` | map of `tenant_id` → `RateLimitConfig` | Per-tenant token bucket params |
| `state_store` | `std::optional<StateStoreConfig>` | External store backend config |

---

### ShardingConfig (Shuffle Sharding)

| Field | Type | Description |
|-------|------|-------------|
| `num_cells` | `uint32_t` | Total worker cells N (default: 16) |
| `cells_per_tenant` | `uint32_t` | Cells assigned to each tenant K (default: 4) |
| `hash_seed` | `uint64_t` | Seed for consistent hashing ring |

**Assignment algorithm**: For a given `tenant_id`, compute K independent hash values
`H_i(tenant_id, seed_i)` for i in [0, K). Each hash selects a cell from [0, N) without
replacement (retry on collision). The same tenant always maps to the same K cells; two tenants
share at most K cells in the worst case, and on average share K²/N cells.

**Blast radius**: A fully-saturated tenant affects at most K/N = 25% of cells (default). The
remaining (N-K)/N = 75% of cells serve other tenants unaffected.

---

### RateLimitConfig (Per-Tenant Token Bucket)

| Field | Type | Description |
|-------|------|-------------|
| `capacity` | `uint32_t` | Maximum burst size (tokens) |
| `refill_rate` | `double` | Tokens added per second |
| `overflow_action` | `OverflowAction` | `Reject` (emit degraded decision) or `Drop` (no decision) |

---

## Stage Failure Mode Definitions

| Stage | Default FailureMode | Rationale |
|-------|--------------------|-|
| Event Ingest | `EmitDegraded` | Pipeline MUST always emit a decision; ingest failure is audit-logged |
| Lightweight Evaluation | `FailOpen` | False negatives preferred over blocking valid traffic on transient evaluator errors |
| ML Inference | `FailOpen` | Model timeouts are common; pipeline must not stall on slow inference |
| Policy Evaluation | `FailClosed` | Policy failures should be conservative; unknown state → deny |
| Decision Emission | `EmitDegraded` | Decision is always produced; emission target failure is retried async |

---

## State Transitions: Pipeline Lifecycle

```
Stopped ──start()──► Starting ──ready──► Running
                                           │
                              drain()◄─────┤
                                │          │ stop()
                                ▼          ▼
                             Draining ──► Stopped
                             (finish in-flight events; reject new)
```

- Transitions are atomic; concurrent `start()` calls after `Starting` are no-ops.
- `drain()` sets a deadline after which in-flight events receive a `PipelineShuttingDown`
  degraded decision and are emitted immediately.

---

## External StateStore Contract

The pluggable external state backend satisfies the `StateStore` concept:

```
concept StateStore<S>:
  - get(WindowKey) -> expected<WindowValue, StoreError>
  - compare_and_swap(WindowKey, expected_value, new_value) -> expected<bool, StoreError>
  - expire(WindowKey) -> expected<void, StoreError>
  - is_available() -> bool   // non-blocking health probe
```

On `StoreError` or `!is_available()`, the pipeline falls back to in-process state and sets
`DegradedReason::StateStoreUnavailable` in the decision audit trail.
