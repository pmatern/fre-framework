# Feature Specification: DuckDB/Parquet External Storage

**Feature Branch**: `004-duckdb-external-storage`
**Created**: 2026-03-17
**Status**: Implemented
**Input**: Replace the `ExternalStoreBackend` stub with an embedded DuckDB store that persists window state to local disk, archives old epochs to Parquet, and supports long-horizon aggregate queries (e.g., 30-day lookback) without touching external infrastructure.

## Clarifications

### Session 2026-03-17

- Q: DuckDB or chdb? → A: DuckDB — stable C API, CMake-native amalgamation build, ACID transactions (CAS), direct `read_parquet()`, MIT license. chdb is Python-first with no stable C API or CAS support.
- Q: Should DuckDB be mandatory or optional? → A: Optional (`FRE_ENABLE_DUCKDB=OFF` by default); guarded by `#ifdef FRE_ENABLE_DUCKDB` throughout.
- Q: How is the existing `ExternalWindowStore` fallback wired? → A: `DuckDbWindowStore::as_backend()` fills the `ExternalStoreBackend` vtable; `ExternalWindowStore` fallback logic requires zero changes.
- Q: Should the DuckDB store be used on the hot evaluation path? → A: Warm-tier CAS operations are on the hot path (<10ms budget). `query_range()` is explicitly NOT on the hot path — must be called asynchronously.

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Warm-Tier CAS State Storage (Priority: P1)

Evaluators that use `ExternalWindowStore` backed by DuckDB can read and write window state with optimistic concurrency (compare-and-swap). State survives process restarts because DuckDB writes a WAL-backed on-disk database file.

**Why this priority**: This is the foundational durability guarantee. Without it, all state is lost on restart and the external store is no better than `InProcessWindowStore`.

**Independent Test**: Create a `DuckDbWindowStore`, write a value via CAS, open a second instance on the same file, read back the value.

**Acceptance Scenarios**:

1. **Given** a `DuckDbWindowStore` with a non-empty `db_path`, **When** a value is written via `compare_and_swap()` and the store is destroyed and re-opened, **Then** `get()` on the new instance returns the original value.
2. **Given** a CAS with the correct current version, **When** `compare_and_swap()` is called, **Then** it returns `true` and the version increments.
3. **Given** a CAS with a stale version (concurrent writer has already updated), **When** `compare_and_swap()` is called, **Then** it returns `false` and the stored value is unchanged.
4. **Given** a missing key, **When** `get()` is called, **Then** it returns `{aggregate: 0.0, version: 0}` (matching `InProcessWindowStore` semantics).

---

### User Story 2 — Fallback When Database Unavailable (Priority: P2)

When DuckDB cannot open the database file (bad path, disk full, permissions), `is_available()` returns false and `ExternalWindowStore` automatically falls back to `InProcessWindowStore`. Evaluators continue to function — they just lose durability for that session.

**Why this priority**: Operational safety. A broken disk path must not crash the pipeline.

**Independent Test**: Pass an invalid `db_path` to `DuckDbWindowStore`; assert `is_available() == false`; wrap in `ExternalWindowStore`; call `get()`; assert it succeeds and `is_degraded() == true`.

**Acceptance Scenarios**:

1. **Given** a `DuckDbWindowStore` constructed with a path to a non-existent directory, **When** `is_available()` is called, **Then** it returns false.
2. **Given** an `ExternalWindowStore` backed by an unavailable `DuckDbWindowStore`, **When** `get()` is called, **Then** it succeeds via the fallback `InProcessWindowStore` and `is_degraded()` becomes true.

---

### User Story 3 — Long-Horizon Range Queries (Priority: P3)

`DuckDbWindowStore::query_range()` returns the sum of `aggregate` across a range of epochs, unioning the warm-tier DuckDB table with cold-tier Parquet archives. This enables evaluators like `WindowedHistoricalEvaluator` to answer 30-day lookback questions without external OLAP infrastructure.

**Why this priority**: High-value analytical capability; not on the hot path so latency tolerance is higher (< 100ms acceptable).

**Independent Test**: Write values across several epochs, advance past warm-tier retention, trigger flush to Parquet, then call `query_range()` and assert the sum includes both warm and cold data.

**Acceptance Scenarios**:

1. **Given** window values across epochs [10, 20], **When** `query_range(tenant, entity, window, 10, 20)` is called, **Then** it returns the correct aggregate sum.
2. **Given** old epochs flushed to Parquet and deleted from the warm tier, **When** `query_range()` spans both warm and cold epochs, **Then** it returns the combined sum.
3. **Given** no rows matching the query, **When** `query_range()` is called, **Then** it returns `0.0` (not an error).

---

### Edge Cases

- In-memory mode (`db_path = ""`): no WAL file created; `is_available()` returns true; restart recovery not applicable.
- `parquet_archive_dir = ""`: flush thread skips archival; warm tier retains all epochs.
- CAS hash collision within a single transaction: `BEGIN/INSERT OR IGNORE/UPDATE WHERE version=?/COMMIT` pattern handles it atomically.
- Concurrent `query_range()` calls: use a separate read connection protected by a mutex.
- `flush_interval_ms = 0`: disables background flush thread.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: `DuckDbWindowStore` MUST satisfy the `StateStore` concept (`get`, `compare_and_swap`, `expire`, `is_available`).
- **FR-002**: CAS MUST be atomic: `BEGIN; INSERT OR IGNORE ...; UPDATE ... WHERE version=?; COMMIT`. `duckdb_rows_changed() == 0` indicates version mismatch.
- **FR-003**: `is_available()` MUST return false when the database cannot be opened; no exception may propagate.
- **FR-004**: `as_backend()` MUST return an `ExternalStoreBackend` vtable compatible with `ExternalWindowStore`.
- **FR-005**: `query_range()` MUST union warm-tier (DuckDB table) and cold-tier (Parquet archive) results.
- **FR-006**: A background `std::jthread` MUST flush epochs older than `warm_epoch_retention` to `<parquet_archive_dir>/epoch=N/part-0000.parquet` and delete them from the warm tier.
- **FR-007**: `query_range()` MUST use a separate DuckDB connection from the hot-path connection.
- **FR-008**: All DuckDB interactions MUST use the C API (not the C++ API) for ABI stability.
- **FR-009**: The DuckDB amalgamation MUST be built as a static library via CPMAddPackage; no system-installed DuckDB dependency.
- **FR-010**: All DuckDB code MUST be compiled only when `FRE_ENABLE_DUCKDB=ON`.

### Key Entities

- **`DuckDbConfig`**: `db_path`, `parquet_archive_dir`, `flush_interval_ms`, `window_ms`, `warm_epoch_retention`.
- **`DuckDbWindowStore`**: PIMPL; satisfies `StateStore`; exposes `as_backend()` and `query_range()`.
- **`WindowedHistoricalEvaluator<Store>`**: Template evaluator using `query_range()` for long-horizon decisions; satisfies `LightweightEvaluator`.
- **`StoreErrorCode::QueryRangeError`**: New error code for range query failures.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: CAS with correct version succeeds in < 10ms P99 (warm-tier budget).
- **SC-002**: A second `DuckDbWindowStore` opened on the same `db_path` reads back all values written by the first instance (restart recovery).
- **SC-003**: `is_available() == false` for an invalid `db_path`; `ExternalWindowStore` falls back transparently.
- **SC-004**: `query_range()` returns the correct sum across warm and cold tiers.
- **SC-005**: All 001/002 tests continue to pass with `FRE_ENABLE_DUCKDB=OFF` (no regression).

## Assumptions

- DuckDB v1.1.3 amalgamation is used; the C API is the ABI boundary.
- `query_range()` is never called on the hot-path coroutine strand — latency tolerance is ~100ms.
- Parquet archival is best-effort; losing a flush cycle does not corrupt the warm tier.
- DuckDB in-memory mode (`db_path=""`) is sufficient for unit tests; no disk I/O needed in CI.
