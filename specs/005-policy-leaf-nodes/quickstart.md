# Quickstart: Extended Policy Rule Leaf Nodes

All new nodes live in `namespace fre::policy` and are included via `<fre/policy/rule_engine.hpp>`.

## Tag String Matching

```cpp
using namespace fre::policy;

PolicyStageConfig cfg;

// Block if user_agent contains "bot"
cfg.add_rule(
    PolicyRule{TagContains{"user_agent", "bot"}},
    0, Verdict::Block, "block_bots");

// Flag requests from any US region
cfg.add_rule(
    PolicyRule{TagStartsWith{"country", "US"}},
    1, Verdict::Flag, "flag_us_traffic");

// Block known high-risk tiers
cfg.add_rule(
    PolicyRule{TagIn{"risk_tier", {"high", "critical"}}},
    0, Verdict::Block, "block_high_risk");

// Flag events that have no session (presence check)
cfg.add_rule(
    PolicyRule{Not{TagExists{"session_id"}}},
    2, Verdict::Flag, "flag_sessionless");
```

## Numeric Tag Comparisons

```cpp
// Block if request_count tag exceeds 1000
cfg.add_rule(
    PolicyRule{TagValueGreaterThan{"request_count", 1000.0}},
    0, Verdict::Block, "block_high_volume");

// Flag if error_rate tag is in suspicious mid-range
cfg.add_rule(
    PolicyRule{TagValueBetween{"error_rate", 0.05, 0.20}},
    1, Verdict::Flag, "flag_elevated_error_rate");

// Flag if latency_ms tag is unexpectedly low (possible spoofing)
cfg.add_rule(
    PolicyRule{TagValueLessThan{"latency_ms", 1.0}},
    1, Verdict::Flag, "flag_suspicious_latency");
```

## Event Field Matching

```cpp
// Block login attempts from a specific tenant (e.g., during lockout)
cfg.add_rule(
    PolicyRule{And{
        TenantIs{"locked-tenant"},
        EventTypeIs{"login_attempt"}
    }},
    0, Verdict::Block, "block_locked_tenant_login");

// Flag any of several high-risk event types
cfg.add_rule(
    PolicyRule{EventTypeIn{{"password_reset", "mfa_disable", "export_data"}}},
    1, Verdict::Flag, "flag_sensitive_operations");

// Flag stale events (may indicate replay attack)
cfg.add_rule(
    PolicyRule{EventOlderThan{std::chrono::seconds{30}}},
    1, Verdict::Flag, "flag_stale_event");
```

## Evaluator Score Range and Pipeline Health

```cpp
// Block if anomaly score is in the high-confidence range
cfg.add_rule(
    PolicyRule{EvaluatorScoreBetween{"anomaly_model", 0.85f, 1.0f}},
    0, Verdict::Block, "block_high_anomaly");

// Block defensively if inference was skipped (timed out)
cfg.add_rule(
    PolicyRule{EvaluatorWasSkipped{"anomaly_model"}},
    0, Verdict::Block, "block_on_inference_skip");

// Flag if inference stage itself ran degraded
cfg.add_rule(
    PolicyRule{StageIsDegraded{"inference"}},
    1, Verdict::Flag, "flag_degraded_inference");

// Flag events explicitly labelled by the allow-list as unknown
cfg.add_rule(
    PolicyRule{EvaluatorReasonIs{"allow_list", "unknown_entity"}},
    2, Verdict::Flag, "flag_unknown_entity");
```

## Composing New Nodes with Existing Ones

New and existing leaf nodes compose freely:

```cpp
// Block if: (eval flagged OR anomaly score high) AND inference didn't skip AND not staging
cfg.add_rule(
    PolicyRule{And{
        And{
            Or{
                StageVerdictIs{"eval", Verdict::Flag},
                EvaluatorScoreBetween{"anomaly_model", 0.8f, 1.0f}
            },
            Not{EvaluatorWasSkipped{"anomaly_model"}}
        },
        Not{TagEquals{"env", "staging"}}
    }},
    0, Verdict::Block, "block_confident_threat");
```
