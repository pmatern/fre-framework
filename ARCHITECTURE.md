# fre-framework Architecture Guide

> Near Real-Time Detection Pipeline Framework — C++23

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Start](#2-quick-start)
   - [Minimal Pipeline](#21-minimal-pipeline)
   - [Synchronous Submit](#22-synchronous-submit)
   - [Full ML Pipeline](#23-full-ml-pipeline)
3. [Configuration Reference](#3-configuration-reference)
   - [PipelineConfig Builder](#31-pipelineconfig-builder)
   - [Stage Configs](#32-stage-configs)
   - [Sharding & Rate Limiting](#33-sharding--rate-limiting)
   - [Logging](#34-logging)
   - [Build Flags & Presets](#35-build-flags--presets)
4. [Internal Architecture](#4-internal-architecture)
   - [Pipeline Execution Model](#41-pipeline-execution-model)
   - [Multi-Tenant Shuffle Sharding](#42-multi-tenant-shuffle-sharding)
   - [Rate Limiting](#43-rate-limiting)
   - [Stage Internals](#44-stage-internals)
   - [State & Windowing](#45-state--windowing)
   - [Policy Rule Engine](#46-policy-rule-engine)
   - [Evaluator Plugin System](#47-evaluator-plugin-system)
   - [Service Harness](#48-service-harness)
5. [Key Design Decisions](#5-key-design-decisions)
6. [Error Handling Reference](#6-error-handling-reference)
7. [Testing](#7-testing)

---

## 1. Overview

fre-framework is a C++23 library for building **near real-time event detection pipelines**. It
models event processing as a five-stage directed graph:

```
Event
  │
  ▼
┌─────────┐    ┌──────────────┐    ┌───────────────┐    ┌────────────┐    ┌──────┐
│  ingest │ ──▶│  eval (light)│ ──▶│  inference    │ ──▶│   policy   │ ──▶│ emit │
│         │    │  evaluators  │    │  (ML models)  │    │  rule AST  │    │      │
└─────────┘    └──────────────┘    └───────────────┘    └────────────┘    └──────┘
```

All three middle stages are **optional** — a pipeline needs only ingest + emit. Each stage is
independently configurable with its own timeout, failure mode, and verdict composition rule.

**Core design values:**

- **Pluggable evaluators** — any type satisfying a C++23 concept plugs into any stage.
  Dynamic-load plugins use a stable C vtable ABI.
- **Shuffle-shard multi-tenancy** — each tenant is deterministically pinned to a subset of
  Asio strands (cells). Failures are blast-radius contained.
- **No exceptions, no RTTI** — all fallible operations return `std::expected<T, E>`.
  Predictable latency with no hidden control-flow.
- **Graceful degradation** — stages can fail open (pass + mark degraded) or fail closed
  (block). A `DegradedReason` bitmask captures every concurrent failure.

---

## 2. Quick Start

### 2.1 Minimal Pipeline

Implement the `LightweightEvaluator` concept, wire stages with the builder DSL, and
call `start()` / `submit()` / `drain()`:

```cpp
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/stage/emit_stage.hpp>

using namespace fre;
using namespace std::chrono_literals;

// Any type with this signature satisfies LightweightEvaluator<T>
struct TrustedEntityAllowList {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& event) {
        if (event.entity_id == "trusted-user")
            return EvaluatorResult{.evaluator_id = "allow_list",
                                   .verdict      = Verdict::Pass,
                                   .reason_code  = "trusted_entity"};
        return EvaluatorResult{.evaluator_id = "allow_list",
                               .verdict      = Verdict::Flag,
                               .reason_code  = "unknown_entity"};
    }
};
static_assert(LightweightEvaluator<TrustedEntityAllowList>);

int main() {
    auto cfg = PipelineConfig::Builder()
        .pipeline_id("minimal-example")
        .latency_budget(300ms)
        .ingest(IngestStageConfig{.skew_tolerance = 30s, .timeout = 5ms})
        .lightweight_eval(
            EvalStageConfig{.timeout = 10ms, .failure_mode = FailureMode::FailOpen}
            .add_evaluator(TrustedEntityAllowList{})
        )
        .emit(EmitStageConfig{}.add_stdout_target())
        .build();

    if (!cfg) { /* handle ConfigError */ }

    Pipeline pipeline{std::move(*cfg)};
    pipeline.start();

    pipeline.submit(Event{
        .tenant_id  = "acme",
        .entity_id  = "trusted-user",
        .event_type = "api_call",
        .timestamp  = std::chrono::system_clock::now(),
    });

    pipeline.drain(5s);
}
```

### 2.2 Synchronous Submit

`submit_sync()` blocks until the pipeline emits a `Decision` (or the latency budget
expires). An optional `StopToken` allows external cancellation:

```cpp
#include <fre/pipeline/sync_submit.hpp>

// Simple blocking call — returns std::expected<Decision, SubmitSyncError>
auto result = pipeline.submit_sync(event);
if (!result) {
    switch (result.error()) {
        case SubmitSyncError::Timeout:           /* budget expired */ break;
        case SubmitSyncError::RateLimited:       /* token bucket empty */ break;
        case SubmitSyncError::PipelineUnavailable: /* not Running */ break;
        case SubmitSyncError::Cancelled:         /* stop token fired */ break;
        default: break;
    }
}

// With cancellation
StopSource src;
std::thread canceller([&]{ std::this_thread::sleep_for(50ms); src.request_stop(); });
auto result = pipeline.submit_sync(event, src.get_token());
canceller.join();
```

### 2.3 Full ML Pipeline

A five-stage pipeline with a lightweight evaluator, ONNX-compatible inference stub,
and a declarative policy rule:

```cpp
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/policy/rule_engine.hpp>
#include <fre/testing/pipeline_harness.hpp>

using namespace fre;
using namespace fre::policy;

// Stub — replace with OnnxInferenceEvaluator (FRE_ENABLE_ONNX=ON)
struct AnomalyScoreStub {
    float fixed_score;
    std::expected<EvaluatorResult, EvaluatorError>
    evaluate(const Event&) const noexcept {
        return EvaluatorResult{.evaluator_id = "anomaly_stub",
                               .verdict      = Verdict::Pass,
                               .score        = fixed_score};
    }
};
static_assert(InferenceEvaluator<AnomalyScoreStub>);

// Configure stages
EvalStageConfig eval_cfg;
eval_cfg.add_evaluator([](const Event& ev) -> std::expected<EvaluatorResult, EvaluatorError> {
    return EvaluatorResult{
        .evaluator_id = "suspicious_prefix",
        .verdict = ev.entity_id.contains("suspicious") ? Verdict::Flag : Verdict::Pass,
    };
});

InferenceStageConfig inf_cfg;
inf_cfg.add_evaluator(AnomalyScoreStub{.fixed_score = 0.92f});
inf_cfg.score_threshold = 0.75f;

PolicyStageConfig policy_cfg;
policy_cfg.add_rule(
    PolicyRule{And{StageVerdictIs{"eval", Verdict::Flag},
                   EvaluatorScoreAbove{"anomaly_stub", 0.8f}}},
    /*priority=*/1, Verdict::Block, "block_suspicious_high_anomaly");

auto cfg = PipelineConfig::Builder{}
    .pipeline_id("ml-example")
    .eval_config(std::move(eval_cfg))
    .inference_config(std::move(inf_cfg))
    .policy_config(std::move(policy_cfg))
    .emit_config(EmitStageConfig{}.add_stdout_target())
    .build();

// PipelineTestHarness collects decisions synchronously — useful for tests and examples
fre::testing::PipelineTestHarness harness{std::move(*cfg)};
harness.start();
harness.submit_events(std::span<const Event>{events.data(), events.size()});
auto decisions = harness.wait_for_decisions(events.size(), 5000ms);
harness.drain(3s);
```

---

## 3. Configuration Reference

### 3.1 PipelineConfig Builder

`PipelineConfig::Builder` (in `include/fre/pipeline/pipeline_config.hpp`) is a fluent
DSL that validates all constraints before constructing the config.

| Builder method | Type | Default | Notes |
|---|---|---|---|
| `.pipeline_id(str)` | `std::string` | required | Unique pipeline identifier |
| `.pipeline_version(str)` | `std::string` | `"1.0.0"` | Embedded in every Decision |
| `.latency_budget(dur)` | `milliseconds` | `300ms` | Sum of all stage timeouts must fit |
| `.ingest(cfg)` | `IngestStageConfig` | — | See §3.2 |
| `.lightweight_eval(cfg)` | `EvalStageConfig` | optional | Lightweight evaluator stage |
| `.inference_config(cfg)` | `InferenceStageConfig` | optional | ML inference stage |
| `.policy_config(cfg)` | `PolicyStageConfig` | optional | Declarative rule stage |
| `.emit(cfg)` / `.emit_config(cfg)` | `EmitStageConfig` | **required** | At least one target |
| `.sharding(cfg)` | `ShardingConfig` | — | See §3.3 |
| `.rate_limit(cfg)` | `RateLimitConfig` | — | See §3.3 |

Validation catches: missing emit targets, stage timeout sum exceeding latency budget,
sharding K ≥ N, policy rule references to undefined stage IDs.

### 3.2 Stage Configs

#### IngestStageConfig

```cpp
struct IngestStageConfig {
    std::chrono::milliseconds skew_tolerance = 30s;   // max clock skew before Flag
    std::chrono::milliseconds timeout        = 5ms;
};
```

#### EvalStageConfig

```cpp
struct EvalStageConfig {
    std::chrono::milliseconds timeout        = 10ms;
    FailureMode               failure_mode   = FailureMode::FailOpen;
    CompositionRule           composition    = CompositionRule::AnyBlock;
    // add_evaluator<E>(e) — E must satisfy LightweightEvaluator<E>
    // Also accepts lambdas with matching signature
};
```

**FailureMode values:**

| Value | Behaviour |
|---|---|
| `FailOpen` | Evaluator error → Pass verdict, mark `EvaluatorError` degraded |
| `FailClosed` | Evaluator error → Block verdict |
| `EmitDegraded` | Continue pipeline, emit decision with degraded flag |

**CompositionRule values (eval & inference):**

| Value | Verdict logic |
|---|---|
| `AnyBlock` | Block if any evaluator returns Block |
| `AnyFlag` | Flag if any returns Flag |
| `Unanimous` | Block only if all return Block |
| `Majority` | Block if >50% return Block |
| `WeightedScore` | Inference only: average score vs `score_threshold` |

#### InferenceStageConfig

```cpp
struct InferenceStageConfig {
    std::chrono::milliseconds timeout          = 200ms;
    FailureMode               failure_mode     = FailureMode::FailOpen;
    CompositionRule           composition      = CompositionRule::WeightedScore;
    float                     score_threshold  = 0.5f;   // [0.0, 1.0]
    // add_evaluator<E>(e) — E must satisfy InferenceEvaluator<E>
};
```

Each inference evaluator runs in a **dedicated thread** with timeout enforcement.
A detached thread is left running if it overruns the deadline (avoids blocking shutdown).

#### PolicyStageConfig

```cpp
struct PolicyStageConfig {
    std::chrono::milliseconds timeout      = 20ms;
    FailureMode               failure_mode = FailureMode::FailClosed;  // safe default
    // add_rule(PolicyRule, priority, Verdict, rule_id)
    // Rules sorted by descending priority; first match wins
};
```

#### EmitStageConfig

```cpp
struct EmitStageConfig {
    std::chrono::milliseconds timeout       = 10ms;
    FailureMode               failure_mode  = FailureMode::EmitDegraded;
    uint32_t                  retry_limit   = 3;    // exponential backoff: 10ms, 20ms, 40ms…
    // add_target<E>(shared_ptr<E>) — E must satisfy EmissionTarget<E>
    // add_stdout_target()          — prints to stdout (debug/dev)
    // add_noop_target()            — silently discards
};
```

### 3.3 Sharding & Rate Limiting

```cpp
struct ShardingConfig {
    uint32_t num_cells       = 16;    // must be power of 2; Asio strands created
    uint32_t cells_per_tenant = 4;   // K in Vogels K-of-N; must be < num_cells
    uint32_t thread_count    = 0;    // 0 = std::thread::hardware_concurrency()
};

struct RateLimitConfig {
    int64_t bucket_capacity  = 1000;  // burst ceiling (tokens)
    int64_t tokens_per_second = 500; // steady-state refill rate
    int32_t max_concurrent   = 100;  // max in-flight events per tenant
};
```

### 3.4 Logging

```cpp
#include <fre/core/logging.hpp>

LogConfig log_cfg{
    .diagnostic_level     = LogLevel::Info,
    .diagnostic_file_path = "/var/log/fre/pipeline.log",  // empty = console only
    .audit_file_path      = "/var/log/fre/audit.ndjson",  // NDJSON per-decision record
    .rotate_max_bytes     = 100 * 1024 * 1024,            // 100 MiB
    .rotate_file_count    = 5,
};
fre::init_logging(log_cfg);
```

Log macros: `FRE_LOG_TRACE`, `FRE_LOG_DEBUG`, `FRE_LOG_INFO`, `FRE_LOG_WARNING`,
`FRE_LOG_ERROR`. Backed by quill 4.x lock-free SPSC — safe on the hot-path strand.

`fre::log_audit(decision)` writes one NDJSON record to the audit log for every emitted
`Decision`.

### 3.5 Build Flags & Presets

**CMake feature flags:**

| Flag | Default | Effect |
|---|---|---|
| `FRE_ENABLE_ONNX=ON` | OFF | Builds `OnnxInferenceEvaluator` (requires ONNX Runtime pre-installed) |
| `FRE_ENABLE_DUCKDB=ON` | OFF | Builds `DuckDbWindowStore` (CPM fetches DuckDB 1.1.3 amalgamation) |
| `FRE_BUILD_TESTS=ON` | ON (debug) | Builds Catch2 test suite |
| `FRE_BUILD_EXAMPLES=ON` | ON (debug) | Builds example binaries |
| `FRE_BUILD_SERVICE=ON` | OFF | Builds `fre-service` HTTP harness binary |
| `FRE_CLANG_TIDY=ON` | OFF | Enables clang-tidy with warnings-as-errors |

**CMake presets** (output directory: `build/<preset>/`):

| Preset | Build type | Tests | Purpose |
|---|---|---|---|
| `debug` | Debug | ON | Development, no optimisation |
| `release` | Release | OFF | `-O3` + LTO |
| `relwithdebinfo` | RelWithDebInfo | ON | Balanced default |
| `ci` | RelWithDebInfo | ON | clang-tidy strict, errors-as-errors |
| `coverage` | Debug | ON | lcov/llvm-cov instrumentation |
| `asan` | Debug | ON | AddressSanitizer + LeakSanitizer |
| `ubsan` | Debug | ON | UndefinedBehaviorSanitizer |
| `tsan` | Debug | ON | ThreadSanitizer |
| `duckdb` | Debug | ON | Debug + DuckDB backend |
| `duckdb-release` | Release | OFF | Release + DuckDB backend |

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

---

## 4. Internal Architecture

### 4.1 Pipeline Execution Model

**State machine** (`src/pipeline/pipeline.cpp`):

```
Stopped ──start()──▶ Starting ──ready──▶ Running ──drain()──▶ Draining ──▶ Stopped
```

`Pipeline::Impl` holds:
- An `asio::thread_pool` (sized by `ShardingConfig::thread_count`)
- A `TenantRouter` that maps tenant IDs to strands
- An atomic `PipelineState` for thread-safe observation

**Async path — `submit()`:**
Events are dispatched via `asio::co_spawn` onto the tenant's assigned strand. The
coroutine `run_event()` processes all five stages sequentially, accumulating
`StageOutput` records into a `Decision`. All stages return
`std::expected<StageOutput, Error>`; on an unexpected error, the coroutine emits a
degraded `Decision` and returns.

```
submit(event)
  └──▶ TenantRouter::try_acquire()        [rate limit check]
  └──▶ co_spawn on tenant strand
         └──▶ run_event()
                ├── IngestStage::process(event)
                ├── EvalStage::process(event)       [optional]
                ├── InferenceStage::process(event)  [optional]
                ├── PolicyStage::process(ctx)        [optional]
                └── EmitStage::process(decision)
```

**Sync path — `submit_sync()`:**
A `SyncContext` holds a `std::mutex` + `std::condition_variable` and a captured emission
target. `submit_sync()` injects a capturing target into the emit stage, submits the event
async, then blocks waiting on the condition variable until either the decision arrives,
the latency budget expires, or the `StopToken` fires.

### 4.2 Multi-Tenant Shuffle Sharding

**Algorithm** (Vogels 2014 K-of-N, `src/sharding/tenant_router.cpp`):

For each tenant, K cells are chosen from N without replacement. Each seed `s_i` from
`include/fre/sharding/hash_seeds.hpp` produces one candidate cell:

```
candidate_i = hash(tenant_id, s_i) % N
```

If `candidate_i` was already selected, the loop increments the candidate modulo N until
finding a unique slot. The result is a deterministic, stable set of K strands for each
tenant.

**Why non-power-of-2 fleet sizes:**
With `N = 2^k`, the modulo operation keeps bias small but not zero. A prime or
semi-prime N (e.g. 31) distributes candidates more uniformly across cells, reducing the
probability that two high-traffic tenants share multiple cells.

**Fleet-level routing** (`include/fre/service/fleet_router.hpp`):
`FleetRouter` runs the identical algorithm substituting service instance IDs for strand
IDs. When an HTTP request arrives for a tenant the local instance does not own, it
returns HTTP 503 with header `X-Fre-Redirect-Hint: host:port,host:port` listing owner
addresses from the topology.

**Critical:** the seeds in `hash_seeds.hpp` are **frozen**. Changing them re-maps every
tenant to different cells/instances, invalidating all windowed state.

### 4.3 Rate Limiting

Each tenant has a `TenantState` with a lock-free token bucket and a concurrency counter.

**Token bucket** (fixed-point arithmetic, 1024× scale):
- Capacity and refill rate stored as `int64_t * 1024`
- Refill amount = `tokens_per_second * elapsed_ns / 1e9 * 1024`, clamped to capacity
- CAS loop on `bucket_tokens` — no mutex, no syscall

**Dual cap:**
- `try_acquire()` fails if the bucket has < 1024 tokens **or** `in_flight` ≥
  `max_concurrent`
- `release()` (via `TenantConcurrencyGuard` RAII) decrements `in_flight`

### 4.4 Stage Internals

#### Ingest Stage (`src/stage/ingest_stage.cpp`)

1. Calls `event.is_valid()` — tenant_id and entity_id must be non-empty
2. Computes `now - event.timestamp`; if > `skew_tolerance` → Flag verdict +
   `IngestValidationFailed` degraded reason

#### Eval Stage (`src/stage/eval_stage.cpp`)

Evaluators run **sequentially** in registration order. For each evaluator:
- On success: record `EvaluatorResult`, apply `CompositionRule`
- On error: apply `FailureMode` (FailOpen → skip + degrade, FailClosed → Block)

Composition is applied after each evaluator, short-circuiting on Block for `AnyBlock`.

#### Inference Stage (`src/stage/inference_stage.cpp`)

Each evaluator is spawned in a **dedicated `std::thread`** with a `std::promise`.
The main coroutine waits on the `std::future` with `wait_for(timeout)`. On timeout, the
thread is detached (not joined) and the evaluator result is marked as timed-out degraded.

`WeightedScore` composition averages the `score` fields of all non-skipped results and
compares against `score_threshold` to derive the stage verdict.

#### Policy Stage (`src/stage/policy_stage.cpp`)

Iterates rules sorted by descending priority. The first rule for which
`RuleEngine::evaluate(ctx, rule)` returns `true` determines the stage verdict. The
`PolicyContext` contains references to all prior `StageOutput` records.

#### Emit Stage (`src/stage/emit_stage.cpp`)

Retry loop over all registered emission targets:
- Attempt → on `EmissionError`: wait `10ms * 2^attempt`, retry up to `retry_limit`
- After exhaustion: increment `dropped_decisions`, mark `EmissionRetryExhausted`
- Emission failure does **not** change the final `Verdict`

### 4.5 State & Windowing

#### InProcessWindowStore (`include/fre/state/window_store.hpp`)

A sharded hash map (`num_shards`, default 64) where each shard has its own mutex.
Key format: `"tenant:entity:window:epoch"`. Shard index:
`(hash(tenant_id) ^ hash(entity_id)) % num_shards`.

**Atomic increment pattern** (`ThresholdEvaluator<Store>`):
```
loop up to 10 times:
    value = store.get(key)
    new_value = {value.aggregate + delta, value.version + 1}
    if store.compare_and_swap(key, value, new_value):
        break
```

#### ExternalWindowStore (`include/fre/state/external_store.hpp`)

Wraps an `ExternalStoreBackend` (function-pointer table). On availability failure
(`is_available() → false`), automatically falls back to the injected
`InProcessWindowStore`. Tracks `using_fallback_` for `is_degraded()` signalling.

#### WriteBackWindowStore (`include/fre/state/write_back_window_store.hpp`)

**Recommended pattern** for DuckDB-backed pipelines (opt-in, `FRE_ENABLE_DUCKDB=ON`).
`InProcessWindowStore` is the authoritative hot-path store; DuckDB is written
asynchronously in the background.

```
Hot path (Asio strand):
  get() / compare_and_swap()  →  InProcessWindowStore only  (< 1ms always)
  Successful CAS              →  insert key into dirty_set_

Background jthread (every flush_interval_ms):
  1. swap(dirty_set_, empty)         — brief lock, new CAS calls re-insert freely
  2. read current value per key from InProcessWindowStore
  3. DuckDbWindowStore::upsert_batch()  — single BEGIN / N×INSERT ON CONFLICT / COMMIT

Startup:
  DuckDbWindowStore::scan_warm_tier()  →  seed InProcessWindowStore
  dirty_set_ left empty (reads-in, not new writes)
```

`is_available()` always returns `true` — the primary is in-memory and never fails.
Dirty entries accumulate silently if DuckDB is unavailable and retry each flush cycle.

`query_range()` calls `flush_sync()` first, then delegates to `DuckDbWindowStore`.
Acceptable because `query_range()` is off-hot-path with a 100ms tolerance.

```cpp
// Recommended: WriteBackWindowStore
auto warm = std::make_shared<DuckDbWindowStore>(DuckDbConfig{
    .db_path             = "/var/lib/fre/state.db",
    .parquet_archive_dir = "/var/lib/fre/archive",
    .flush_interval_ms   = 0,         // WriteBackWindowStore drives flushes
    .window_ms           = 60000,
    .warm_epoch_retention = 3,
});
auto primary = std::make_shared<InProcessWindowStore>();
auto store   = std::make_shared<WriteBackWindowStore>(
    primary, warm, WriteBackConfig{.flush_interval_ms = 500});

// ThresholdEvaluator is now a template on StateStore:
ThresholdEvaluator evaluator{config, store};   // deduces ThresholdEvaluator<WriteBackWindowStore>
```

#### DuckDbWindowStore (`include/fre/state/duckdb_window_store.hpp`)

Two-tier storage backend (warm + cold). Normally used as the `warm_` member of
`WriteBackWindowStore` rather than directly on the hot path.

| Tier | Storage | Access pattern |
|---|---|---|
| **Warm** | DuckDB persistent DB file (WAL) | `upsert_batch()` from WriteBackWindowStore flush |
| **Cold** | Parquet files in `parquet_archive_dir` | Offline analytics only |

`query_range(tenant, entity, window, epoch_start, epoch_end)` sums across warm + cold
tiers. **Never call this on the hot-path Asio strand** — it blocks on DuckDB I/O.
`WriteBackWindowStore::query_range()` calls `flush_sync()` first and is safe to use
from analysis or background threads with a 100ms tolerance.

`DuckDbWindowStore::as_backend()` returns an `ExternalStoreBackend` for use with the
older `ExternalWindowStore` wrapper (still supported for compatibility).

### 4.6 Policy Rule Engine

Rules are expressed as an **AST** using `std::variant` with recursive `std::unique_ptr`
children (`include/fre/policy/rule_engine.hpp`):

```
PolicyRule
  └── Variant: StageVerdictIs | EvaluatorScoreAbove | TagEquals | And | Or | Not
                                                                   │    │    │
                                                             left──┘    │    └──expr
                                                             right──────┘
```

**Leaf nodes:**

*Stage/evaluator results:*

| Node | Evaluates to true when |
|---|---|
| `StageVerdictIs{stage_id, verdict}` | Named stage output matches verdict |
| `EvaluatorScoreAbove{evaluator_id, threshold}` | Named evaluator score > threshold |
| `EvaluatorScoreBetween{id, lo, hi}` | Score in `[lo, hi]` |
| `EvaluatorWasSkipped{evaluator_id}` | Evaluator did not run (degraded/timeout) |
| `EvaluatorReasonIs{evaluator_id, reason}` | Evaluator `reason_code` matches |
| `StageIsDegraded{stage_id}` | Stage has any degraded flag set |

*Tag matching:*

| Node | Evaluates to true when |
|---|---|
| `TagEquals{key, value}` | Event tag `key` has exact value `value` |
| `TagContains{key, substring}` | Tag value contains substring |
| `TagStartsWith{key, prefix}` | Tag value starts with prefix |
| `TagIn{key, {v1,v2,…}}` | Tag value is in the set |
| `TagExists{key}` | Tag key is present (any value) |
| `TagValueLessThan{key, n}` | Tag value parsed as double < `n` |
| `TagValueGreaterThan{key, n}` | Tag value parsed as double > `n` |
| `TagValueBetween{key, lo, hi}` | Tag value parsed as double in `[lo, hi]` |

*First-class Event fields:*

| Node | Evaluates to true when |
|---|---|
| `EventTypeIs{type}` | `event.event_type == type` |
| `EventTypeIn{types}` | `event.event_type` is in the set |
| `TenantIs{tenant_id}` | `event.tenant_id == tenant_id` |
| `EventOlderThan{ms}` | Event age > `ms` milliseconds |
| `EventNewerThan{ms}` | Event age < `ms` milliseconds |

**Composite nodes:** `And`, `Or`, `Not` — all short-circuit correctly.

`RuleEngine::evaluate()` uses `std::visit` with `std::decay_t` dispatch, walking the
AST recursively. Copy constructors handle the `unique_ptr` children.

### 4.7 Evaluator Plugin System

**Same-binary evaluators** implement the C++23 concept directly:

```cpp
// LightweightEvaluator requires:
std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event&);

// InferenceEvaluator adds optional batch:
std::vector<std::expected<EvaluatorResult, EvaluatorError>>
evaluate_batch(std::span<const Event* const> events);

// PolicyEvaluator requires:
std::expected<EvaluatorResult, EvaluatorError> evaluate(const PolicyContext&);

// EmissionTarget requires:
std::expected<void, EmissionError> emit(Decision);
```

**Dynamic plugins** (`include/fre/evaluator/plugin_abi.h`):

```c
typedef struct fre_evaluator_vtable {
    uint32_t    abi_version;    // must == FRE_EVALUATOR_ABI_VERSION (1)
    const char* evaluator_id;
    void*       ctx;
    int (*evaluate)(void* ctx, const fre_event_view_t* event, fre_evaluator_result_t* out);
    void (*destroy)(void* ctx);
} fre_evaluator_vtable_t;

// Export this symbol from your .so / .dll:
typedef fre_evaluator_vtable_t* (*fre_create_evaluator_fn)(const char* config_json);
```

Loading:
```cpp
auto handle = PluginLoader::load("/path/to/evaluator.so", R"({"threshold":0.9})");
if (handle) {
    eval_cfg.add_evaluator(std::move(*handle));
}
```

`EvaluatorHandle` is an RAII wrapper that calls `vtable->destroy(ctx)` on destruction
and performs ABI version checking at load time.

### 4.8 Service Harness

The optional `fre-service` binary (`service/`) wraps a `Pipeline` with a bare-socket
HTTP/1.1 server (Asio acceptor, one `std::thread` per connection).

**Endpoints:**

| Method | Path | Notes |
|---|---|---|
| `POST /events` | Submit one event (JSON body) | 202 Accepted; 503 if not fleet owner |
| `GET /health` | Pipeline state JSON | `{"state":"running"}` |
| `GET /topology` | Fleet topology JSON | 404 if fleet disabled |
| `POST /pipeline/drain` | Graceful shutdown | Blocks until drained |

**Fleet-aware rejection:** before processing a `POST /events`, the harness checks
`FleetRouter::owns(tenant_id)`. On non-ownership it returns:
```
HTTP/1.1 503 Service Unavailable
X-Fre-Redirect-Hint: host1:8080,host2:8080
```

**Environment variables** (service harness only):

| Variable | Default | Meaning |
|---|---|---|
| `FRE_INSTANCE_ID` | `0` | This instance's index in the fleet |
| `FRE_FLEET_SIZE` | `1` | Total number of instances |
| `FRE_INSTANCES_PER_TENANT` | `2` | K in fleet K-of-N |
| `FRE_TOPOLOGY_FILE` | — | JSON file with `[{"id":0,"address":"host:port"},…]` |

---

## 5. Key Design Decisions

### No Exceptions, No RTTI

All fallible operations return `std::expected<T, E>`. Rationale:
- **Predictable latency** — no unwinding cost on the hot-path strand
- **Plugin ABI safety** — C vtable boundary cannot propagate C++ exceptions
- **No hidden control flow** — every failure path is explicit and auditable

`dynamic_cast`, `typeid`, and `std::any` are prohibited. Polymorphism is achieved
via C++23 concepts and `std::visit` over `std::variant`.

### Failure Modes and Degraded Reason

Three orthogonal responses to stage failure:

| Mode | Verdict impact | Use case |
|---|---|---|
| `FailOpen` | No change (treat as Pass) | Non-critical auxiliary signals |
| `FailClosed` | Force Block | Policy stage (default: safe) |
| `EmitDegraded` | No change, set bitmask | Emit: continue regardless |

`DegradedReason` is a `uint16_t` bitmask — multiple concurrent failures compose with
`|`. Consumers inspect `Decision::is_degraded()` and `Decision::degraded_reason` to
distinguish clean from degraded decisions without examining every `StageOutput`.

### Deterministic Hash Seeds

`include/fre/sharding/hash_seeds.hpp` holds eight `uint32_t` constants used by both
`TenantRouter` (cell assignment) and `FleetRouter` (instance assignment). These values
are **frozen for the lifetime of the deployment**. Changing them is equivalent to
re-sharding the entire fleet: all tenants get new cell assignments, all windowed state
becomes inconsistent, and all redirect hints become stale.

### Off-Hot-Path DuckDB Queries

`WriteBackWindowStore` inverts the original DuckDB relationship: `InProcessWindowStore`
is the authoritative hot-path store and DuckDB is written asynchronously in background
batches. `get()`, `compare_and_swap()`, and `expire()` never contact DuckDB — latency
is unconditionally sub-millisecond regardless of DuckDB health.

`query_range()` (via `WriteBackWindowStore` or `DuckDbWindowStore` directly) performs
multi-tier SQL aggregation and **must not** be called on an Asio strand. It is gated
behind a `flush_sync()` call to ensure dirty in-memory writes are visible, and carries
a 100ms latency tolerance. Use it from analysis threads or background jobs only.

### Strand-Per-Cell Coroutine Executor

Each of the N cells is backed by a single `asio::strand`. All events for a given tenant
are dispatched to the same K strands, round-robin per event. Within one strand, events
execute sequentially with no synchronisation overhead. This makes per-tenant windowed
state updates effectively single-threaded — the CAS loop in `ThresholdEvaluator` is a
correctness measure for cross-tenant sharing, not intra-tenant contention.

---

## 6. Error Handling Reference

All errors are members of `fre::Error` (`include/fre/core/error.hpp`):

```cpp
using Error = std::variant<
    ConfigError,        // Builder validation failures
    EvaluatorError,     // Evaluator returned error
    StoreError,         // Window store operation failed
    EmissionError,      // Emit target returned error
    RateLimitError,     // Token bucket or concurrency cap exceeded
    PipelineError,      // Pipeline state machine violation
    FleetRoutingError   // Tenant not owned by this instance
>;
```

Use `fre::error_message(err)` to get a human-readable string. Use `std::visit` or
`std::get_if` to pattern-match on the variant.

**Error propagation:**

```cpp
// Errors short-circuit through std::expected chains
auto result = pipeline.start()
    .and_then([&] { return pipeline.submit(event); });

if (!result) {
    std::visit([](auto& e) { std::cerr << e.detail << '\n'; }, result.error());
}
```

**Degraded vs fatal:**
- **Degraded**: pipeline continues, `Decision::is_degraded() == true`,
  `Decision::degraded_reason` bitmask is set. Downstream can still act on the verdict.
- **Fatal**: `submit()` / `submit_sync()` return `std::unexpected(Error{...})`.
  The event was not processed.

---

## 7. Testing

### Test Layout

```
tests/
  unit/         # Per-component: event types, evaluators, router, store, fleet
  contract/     # Concept satisfaction (static_assert) + config contract conformance
  integration/  # Full pipeline end-to-end: latency, concurrency, policy, stores
```

### PipelineTestHarness

`include/fre/testing/pipeline_harness.hpp` provides a synchronous wrapper for use in
tests and examples:

```cpp
fre::testing::PipelineTestHarness harness{config};
harness.start();
harness.submit_events(events);                          // async fire-and-forget
auto decisions = harness.wait_for_decisions(n, 5000ms); // blocks until n decisions
harness.drain(5s);
harness.clear_decisions();
```

The harness injects a capturing `EmissionTarget` that appends `Decision` records to an
internal vector, signalling a `condition_variable` on each emission.

### CI Quality Gates

Four blocking gates before merge:

| Gate | Command | Requirement |
|---|---|---|
| Sanitizers | `./scripts/ci/run-sanitizers.sh` | ASan, UBSan, TSan all pass |
| Coverage | `./scripts/ci/check-coverage.sh` | ≥80% line coverage |
| Static analysis | `cmake --preset ci && cmake --build --preset ci` | clang-tidy: 0 warnings |
| Valgrind | `./scripts/ci/run-valgrind.sh` | Advisory (non-blocking) |

Run all sanitizer presets locally:
```bash
for preset in asan ubsan tsan; do
    cmake --preset $preset && cmake --build --preset $preset
    ctest --preset $preset --output-on-failure
done
```
