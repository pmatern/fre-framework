# Feature Specification: Write-Back Window Store

**Feature Branch**: `006-write-back-window-store`
**Created**: 2026-03-28
**Status**: Implemented
**Input**: Analysis of DuckDB hot-path usage in `004-duckdb-external-storage` revealed that
`DuckDbWindowStore` was being called synchronously on the Asio coroutine strand for every
event — one full `BEGIN`/`INSERT OR IGNORE`/`UPDATE`/`COMMIT` transaction per CAS. The
in-process store was the *fallback* but should have been the *primary*.

## Clarifications

### Session 2026-03-28

- Q: Should DuckDB be removed from the hot path entirely? → A: Yes. `InProcessWindowStore` becomes
  the authoritative store for reads and writes. DuckDB is written to asynchronously in batches.
- Q: How does state survive process restarts? → A: Constructor loads warm-tier rows from
  `DuckDbWindowStore::scan_warm_tier()` into the primary before any events arrive. Dirty set
  starts empty — loaded rows are not re-queued for flush.
- Q: What happens when DuckDB is unavailable? → A: `is_available()` always returns `true` (primary
  is in-memory). Dirty entries accumulate in `dirty_set_` and retry on each flush cycle. Hot path
  is never blocked.
- Q: Does `query_range()` need to see dirty in-memory writes? → A: Yes. `query_range()` calls
  `flush_sync()` first, then delegates to `DuckDbWindowStore::query_range()`. Acceptable because
  `query_range()` is off-hot-path with 100ms tolerance.
- Q: What about `ThresholdEvaluator` being hardcoded to `InProcessWindowStore`? → A: Templatize
  on `StateStore Store`. CTAD handles all existing call sites unchanged.

## User Scenarios & Testing

### User Story 1 — Hot-Path Latency (Priority: P1)

**As a** pipeline operator,
**I want** sub-millisecond `get()` and `compare_and_swap()` latency regardless of DuckDB health,
**so that** the detection pipeline meets its latency budget unconditionally.

**Acceptance**: `WriteBackWindowStore::get()` and `compare_and_swap()` never call DuckDB.
`is_available()` always returns `true`. Validated by unit test with unavailable DuckDB instance.

### User Story 2 — Async Persistence (Priority: P1)

**As a** pipeline operator,
**I want** window state to be durably persisted to DuckDB without blocking the hot path,
**so that** counts survive process restarts.

**Acceptance**: After `flush_sync()`, `DuckDbWindowStore::get(key)` returns the value written
via `WriteBackWindowStore::compare_and_swap(key, ...)`. The flush always writes the latest
in-memory value (not the value at dirty-mark time). Validated by unit test.

### User Story 3 — Startup Recovery (Priority: P1)

**As a** pipeline operator,
**I want** window counts to resume from the persisted warm-tier state after a restart,
**so that** a process restart does not reset fraud detection counters to zero.

**Acceptance**: Constructing a new `WriteBackWindowStore` on an existing DuckDB file seeds the
`InProcessWindowStore` so that `get(key)` returns the previously-persisted value without any
intervening CAS. CAS continues from the loaded version number. Validated by integration test
(construct, flush, destroy, reconstruct, verify counts continue).

### User Story 4 — Graceful DuckDB Failure (Priority: P2)

**As a** pipeline operator,
**I want** the pipeline to continue operating when DuckDB is unavailable,
**so that** a disk failure or misconfiguration does not stop event processing.

**Acceptance**: With an unavailable DuckDB backend, all 100 submitted events are processed
and decisions emitted correctly. Dirty entries accumulate silently. Validated by integration test.

## Constraints

- No new runtime dependencies (C++23 + existing DuckDB amalgamation).
- `WriteBackWindowStore` must satisfy both `StateStore` and `RangeQueryStore` concepts.
- `ThresholdEvaluator` template change must not break any existing call sites (CTAD).
- `flush_interval_ms` default: 500ms. Configurable via `WriteBackConfig`.
- Destructor performs a final `flush_sync()` after stopping the background thread.
