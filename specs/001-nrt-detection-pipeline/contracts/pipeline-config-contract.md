# Contract: Pipeline Configuration

**Version**: 1.0.0

## Overview

A `PipelineConfig` is the complete, validated description of one pipeline instance. It is
constructed via a typed C++ builder DSL (no strings-in/strings-out configuration at the
call site). For the optional service harness, a JSON/YAML representation is deserialized
into the same typed structure at startup.

Validation occurs entirely at pipeline `start()` time. No partial or lazy validation.

---

## Typed DSL (C++ builder)

```
PipelineConfig::Builder()
  .pipeline_id("fraud-detection-v1")
  .latency_budget(300ms)
  .sharding(ShardingConfig{.num_cells = 16, .cells_per_tenant = 4})

  .ingest(IngestStageConfig{
      .max_payload_bytes = 65536,
      .skew_tolerance    = 5s,
  })

  .lightweight_eval(EvalStageConfig{
      .timeout         = 10ms,
      .failure_mode    = FailureMode::FailOpen,
      .composition     = CompositionRule::AnyBlock,
  }
  .add_evaluator(AllowDenyEvaluator{...})
  .add_evaluator(ThresholdEvaluator{
      .window_duration = 60s,
      .aggregation     = AggregationFn::Count,
      .group_by        = GroupBy::EntityId,
      .threshold       = 100.0,
  }))

  .ml_inference(InferenceStageConfig{
      .timeout         = 200ms,
      .failure_mode    = FailureMode::FailOpen,
      .composition     = CompositionRule::WeightedScore,
      .score_threshold = 0.75f,
  }
  .add_evaluator(OnnxInferenceEvaluator{.model_path = "models/anomaly_v2.onnx"}))

  .policy_eval(PolicyStageConfig{
      .timeout      = 20ms,
      .failure_mode = FailureMode::FailClosed,
  }
  .add_rule(PolicyRule{
      .rule_id    = "block-high-score-flagged",
      .expression = And(
          StageVerdictIs(StageId::LightweightEval, Verdict::Flag),
          EvaluatorScoreAbove("anomaly_v2", 0.8f)
      ),
      .action   = Verdict::Block,
      .priority = 1,
  }))

  .emit(EmitStageConfig{
      .timeout        = 10ms,
      .failure_mode   = FailureMode::EmitDegraded,
      .retry_limit    = 3,
      .retry_backoff  = ExponentialBackoff{.base = 50ms, .max = 500ms},
  }
  .add_target(KafkaEmissionTarget{...}))

  .build()   // → expected<PipelineConfig, ConfigError>
```

---

## Validation rules enforced at `build()`

| Rule | Error if violated |
|------|------------------|
| `pipeline_id` non-empty | `ConfigError::MissingField` |
| At least `Ingest` and `Emit` stages present | `ConfigError::RequiredStageMissing` |
| Per-stage timeouts sum ≤ `latency_budget` | `ConfigError::LatencyBudgetExceeded` |
| Policy rules reference only stages present in config | `ConfigError::UndefinedStageDependency` |
| `sharding.cells_per_tenant` < `sharding.num_cells` | `ConfigError::InvalidShardingConfig` |
| Every evaluator satisfies its stage's concept | Compile-time concept error (not runtime) |
| `latency_budget` ≥ 1ms and ≤ 10,000ms | `ConfigError::InvalidLatencyBudget` |

---

## Service Harness JSON Schema (for standalone deployment)

The service harness accepts a JSON config file. All fields map 1:1 to the typed DSL above.
Duration values are milliseconds (integers). Enums are lowercase strings.

```json
{
  "pipeline_id": "fraud-detection-v1",
  "latency_budget_ms": 300,
  "sharding": { "num_cells": 16, "cells_per_tenant": 4 },
  "stages": {
    "ingest": {
      "max_payload_bytes": 65536,
      "skew_tolerance_ms": 5000
    },
    "lightweight_eval": {
      "timeout_ms": 10,
      "failure_mode": "fail_open",
      "composition": "any_block",
      "evaluators": [
        {
          "type": "allow_deny",
          "config": { "list_source": "file://lists/trusted.txt", "match": "entity_id" }
        },
        {
          "type": "threshold",
          "config": {
            "window_duration_ms": 60000,
            "aggregation": "count",
            "group_by": "entity_id",
            "threshold": 100.0
          }
        }
      ]
    },
    "ml_inference": {
      "timeout_ms": 200,
      "failure_mode": "fail_open",
      "composition": "weighted_score",
      "score_threshold": 0.75,
      "evaluators": [
        { "type": "onnx", "config": { "model_path": "models/anomaly_v2.onnx" } }
      ]
    },
    "policy_eval": {
      "timeout_ms": 20,
      "failure_mode": "fail_closed",
      "rules": [
        {
          "rule_id": "block-high-score-flagged",
          "priority": 1,
          "action": "block",
          "expression": {
            "and": [
              { "stage_verdict_is": { "stage": "lightweight_eval", "verdict": "flag" } },
              { "score_above": { "evaluator": "anomaly_v2", "threshold": 0.8 } }
            ]
          }
        }
      ]
    },
    "emit": {
      "timeout_ms": 10,
      "failure_mode": "emit_degraded",
      "retry_limit": 3,
      "retry_backoff": { "type": "exponential", "base_ms": 50, "max_ms": 500 },
      "targets": [
        { "type": "kafka", "config": { "bootstrap_servers": "kafka:9092", "topic": "decisions" } }
      ]
    }
  }
}
```

---

## ConfigError enumeration

| Variant | Description |
|---------|-------------|
| `MissingField(field_name)` | Required field absent |
| `RequiredStageMissing(stage_id)` | Mandatory stage (Ingest or Emit) not configured |
| `LatencyBudgetExceeded(allocated_ms, budget_ms)` | Stage timeouts exceed total budget |
| `UndefinedStageDependency(rule_id, stage_id)` | Policy rule references absent stage |
| `InvalidShardingConfig(reason)` | K ≥ N or other invariant violated |
| `InvalidLatencyBudget(value_ms)` | Budget out of range |
| `EvaluatorLoadFailed(evaluator_id, reason)` | Plugin dlopen or init failed |
| `StateStoreUnreachable(backend)` | External store probe failed at startup |
