# Data Model: Realistic Performance Tests with DuckDB

**Branch**: `007-realistic-perf-tests` | **Date**: 2026-03-29

These tests introduce no new data types. The entities below are the existing framework types that the tests wire together. This document captures the relevant fields and relationships for implementation reference.

---

## Event

Source: `include/fre/core/event.hpp`

| Field | Type | Test usage |
|-------|------|-----------|
| `tenant_id` | `std::string_view` | Per-tenant isolation; identifies which rate-limit shard and window key namespace to use |
| `entity_id` | `std::string_view` | Matched against deny list; used as `GroupBy::EntityId` window key in `ThresholdEvaluator` |
| `event_type` | `std::string_view` | Set to `"api_call"` in tests (no evaluator inspects this field in the test pipelines) |
| `timestamp` | `std::chrono::system_clock::time_point` | Must be within the same epoch for all events in a single test run to ensure threshold accumulation |

---

## Decision

Source: `include/fre/core/decision.hpp`

| Field | Type | Test usage |
|-------|------|-----------|
| `tenant_id` | `std::string` | Used in load test per-tenant P99 grouping |
| `entity_id` | `std::string` | Used to identify blocked vs. flagged vs. passing decisions |
| `final_verdict` | `Verdict` | `Pass`, `Flag`, or `Block` — asserted in acceptance scenarios |
| `elapsed_us` | `uint64_t` | Microseconds from submit to emit; collected for p50/p99 computation |

---

## WindowKey

Source: `include/fre/state/window_store.hpp`

| Field | Type | Notes |
|-------|------|-------|
| `tenant_id` | `std::string` | Namespaces counts per tenant |
| `entity_id` | `std::string` | Namespaces counts per entity within a tenant |
| `window_name` | `std::string` | Set to `"default"` by `ThresholdEvaluatorConfig` |
| `epoch` | `uint64_t` | `timestamp_ms / window_duration_ms`; all test events use `std::chrono::system_clock::now()` so they land in the same epoch |

---

## WindowValue

Source: `include/fre/state/window_store.hpp`

| Field | Type | Notes |
|-------|------|-------|
| `aggregate` | `double` | Monotonically increasing count per key; threshold evaluated against this |
| `version` | `uint64_t` | Optimistic CAS version; incremented on each successful swap |

---

## ThresholdEvaluatorConfig

Source: `include/fre/evaluator/threshold_evaluator.hpp`

| Field | Test value | Reason |
|-------|-----------|--------|
| `window_duration` | `60s` | Long enough that all test events fall in one epoch |
| `aggregation` | `AggregationFn::Count` | Count events per entity |
| `group_by` | `GroupBy::EntityId` | Isolate counts per entity, not per tenant |
| `threshold` | `200.0` | Crossed by the high-volume entity; low enough for meaningful flagging without excessive CAS retries |
| `window_name` | `"perf_threshold"` | Unique name avoids collision with other tests sharing the same DuckDB file |

---

## AllowDenyEvaluatorConfig

Source: `include/fre/evaluator/allow_deny_evaluator.hpp`

| Field | Test value | Reason |
|-------|-----------|--------|
| `deny_list_path` | temp file with one entry per run | Written by RAII guard at test start; deleted at teardown |
| `allow_list_path` | empty | No allow-list needed; default verdict is `Pass` |
| `match_field` | `AllowDenyMatchField::EntityId` | Block specific entity IDs |
| `default_verdict` | `Verdict::Pass` | Non-blocked, non-allowed entities pass through to threshold check |

---

## DuckDbConfig

Source: `include/fre/state/duckdb_window_store.hpp`

| Field | Test value | Reason |
|-------|-----------|--------|
| `db_path` | `tmp_dir / "fre_perf_<pid>.duckdb"` | On-disk persistence; unique per process to avoid ctest parallel collision |
| `parquet_archive_dir` | `""` | Archival disabled; warm tier only for test duration |
| `flush_interval_ms` | `0` | Background flush disabled in DuckDbWindowStore itself (WriteBackWindowStore owns the flush) |
| `window_ms` | `60000` | Matches `ThresholdEvaluatorConfig::window_duration` |
| `warm_epoch_retention` | `3` | Default; retains current and 2 prior epochs |

---

## WriteBackConfig

Source: `include/fre/state/write_back_window_store.hpp`

| Field | Benchmark value | Load test value | Reason |
|-------|----------------|----------------|--------|
| `flush_interval_ms` | `200` | `500` | Benchmark: frequent enough to flush during run; Load: matches default, reduces flush contention |

---

## TempFileGuard (test-local RAII)

Defined locally in each test file. Not a framework type.

| Field | Type | Notes |
|-------|------|-------|
| `paths` | `std::vector<std::filesystem::path>` | DuckDB file + deny list file |
| destructor | removes all paths via `std::filesystem::remove` | Runs on scope exit, pass or fail |
