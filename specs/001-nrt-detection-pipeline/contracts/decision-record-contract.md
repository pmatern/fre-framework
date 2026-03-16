# Contract: Decision Record

**Version**: 1.0.0

## Overview

A `Decision` is the immutable output record produced for every event that enters the pipeline,
including events that were processed in a degraded state. The record is the authoritative audit
trail for the pipeline's verdict on one event.

`Decision` records are passed to registered `EmissionTarget` implementations. The in-memory
layout is the canonical form; the service harness serializes to the configured wire format
(JSON default, Cap'n Proto optional for high-throughput deployments).

---

## Field Specification

### Top-level Decision

| Field | Type | Never null? | Description |
|-------|------|-------------|-------------|
| `event_id` | `uint64_t` | ✅ | Sequence ID of the originating Event |
| `tenant_id` | `string` | ✅ | Tenant owning this decision |
| `pipeline_id` | `string` | ✅ | Pipeline that produced this decision |
| `final_verdict` | `Verdict` | ✅ | `pass` / `flag` / `block` |
| `stage_outputs` | `StageOutput[]` | ✅ | One entry per active stage, in execution order |
| `degraded` | `bool` | ✅ | True if any stage was degraded |
| `degraded_reason` | `DegradedReason[]` | ✅ | Empty array if not degraded |
| `processing_started_at` | ISO-8601 timestamp | ✅ | When event entered the pipeline |
| `processing_ended_at` | ISO-8601 timestamp | ✅ | When decision was emitted |
| `elapsed_us` | `uint32_t` | ✅ | End-to-end processing time in microseconds |

### StageOutput

| Field | Type | Never null? | Description |
|-------|------|-------------|-------------|
| `stage_id` | `string` | ✅ | e.g., `"ingest"`, `"lightweight_eval"`, `"ml_inference"` |
| `verdict` | `Verdict` | ✅ | Composed verdict for this stage |
| `evaluator_results` | `EvaluatorResult[]` | ✅ | One per registered evaluator |
| `elapsed_us` | `uint32_t` | ✅ | Microseconds spent in this stage |
| `degraded` | `bool` | ✅ | True if any evaluator in this stage was degraded |
| `degraded_reason` | `DegradedReason[]` | ✅ | Empty if not degraded |

### EvaluatorResult

| Field | Type | Never null? | Description |
|-------|------|-------------|-------------|
| `evaluator_id` | `string` | ✅ | Registered evaluator name |
| `verdict` | `Verdict` | ✅ | Individual evaluator verdict |
| `score` | `float` \| `null` | — | Anomaly score [0.0, 1.0]; present for ML evaluators only |
| `reason_code` | `string` \| `null` | — | Short machine-readable reason; null if not provided |
| `skipped` | `bool` | ✅ | True if evaluator was skipped due to degradation |
| `metadata` | `object` | ✅ | Evaluator-specific key-value pairs (empty object if none) |

---

## JSON Wire Format (service harness default)

```json
{
  "event_id": 100042,
  "tenant_id": "acme-corp",
  "pipeline_id": "fraud-detection-v1",
  "final_verdict": "block",
  "degraded": false,
  "degraded_reason": [],
  "processing_started_at": "2026-03-15T14:23:01.004312Z",
  "processing_ended_at":   "2026-03-15T14:23:01.097441Z",
  "elapsed_us": 93129,
  "stage_outputs": [
    {
      "stage_id": "ingest",
      "verdict": "pass",
      "elapsed_us": 312,
      "degraded": false,
      "degraded_reason": [],
      "evaluator_results": []
    },
    {
      "stage_id": "lightweight_eval",
      "verdict": "flag",
      "elapsed_us": 4218,
      "degraded": false,
      "degraded_reason": [],
      "evaluator_results": [
        {
          "evaluator_id": "allow_deny_list",
          "verdict": "pass",
          "score": null,
          "reason_code": null,
          "skipped": false,
          "metadata": {}
        },
        {
          "evaluator_id": "event_rate_threshold",
          "verdict": "flag",
          "score": null,
          "reason_code": "threshold_exceeded",
          "skipped": false,
          "metadata": { "window_count": 143, "threshold": 100, "window_duration_ms": 60000 }
        }
      ]
    },
    {
      "stage_id": "ml_inference",
      "verdict": "flag",
      "elapsed_us": 84201,
      "degraded": false,
      "degraded_reason": [],
      "evaluator_results": [
        {
          "evaluator_id": "anomaly_v2",
          "verdict": "flag",
          "score": 0.87,
          "reason_code": null,
          "skipped": false,
          "metadata": { "model_version": "2.1.0", "embedding_dim": 128 }
        }
      ]
    },
    {
      "stage_id": "policy_eval",
      "verdict": "block",
      "elapsed_us": 4398,
      "degraded": false,
      "degraded_reason": [],
      "evaluator_results": [
        {
          "evaluator_id": "block-high-score-flagged",
          "verdict": "block",
          "score": null,
          "reason_code": "rule_matched",
          "skipped": false,
          "metadata": { "matched_rule": "block-high-score-flagged", "priority": 1 }
        }
      ]
    }
  ]
}
```

---

## Degraded Decision Example

When a stage is degraded (e.g., ML inference timeout with FailOpen), the decision is still
emitted with `degraded: true` and a populated `degraded_reason`.

```json
{
  "event_id": 100043,
  "tenant_id": "acme-corp",
  "pipeline_id": "fraud-detection-v1",
  "final_verdict": "pass",
  "degraded": true,
  "degraded_reason": ["evaluator_timeout"],
  "elapsed_us": 204891,
  "stage_outputs": [
    {
      "stage_id": "ml_inference",
      "verdict": "pass",
      "elapsed_us": 200041,
      "degraded": true,
      "degraded_reason": ["evaluator_timeout"],
      "evaluator_results": [
        {
          "evaluator_id": "anomaly_v2",
          "verdict": "pass",
          "score": null,
          "reason_code": "timeout",
          "skipped": true,
          "metadata": { "timeout_budget_ms": 200 }
        }
      ]
    }
  ]
}
```

---

## Invariants

- Every event entering the pipeline MUST produce exactly one `Decision`.
- A `Decision` is emitted even if the pipeline is shutting down (`PipelineShuttingDown`
  degraded reason).
- `final_verdict` is derived as the most severe verdict across all stage `verdict` fields:
  Block > Flag > Pass.
- `elapsed_us` MUST equal `processing_ended_at - processing_started_at` within ±1µs of
  clock resolution.
- `degraded` at the top level MUST equal the logical OR of all stage `degraded` fields.
