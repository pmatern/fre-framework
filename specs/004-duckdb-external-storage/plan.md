# Implementation Plan: DuckDB/Parquet External Storage

**Branch**: `004-duckdb-external-storage` | **Date**: 2026-03-17 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/004-duckdb-external-storage/spec.md`

## Summary

Replace the `ExternalStoreBackend` stub with `DuckDbWindowStore` — an embedded DuckDB store that uses ACID transactions for CAS, archives old epochs to Parquet, and exposes a `query_range()` method for long-horizon analytical evaluators. Integrated via the existing `ExternalWindowStore` fallback chain; DuckDB is an opt-in CMake option.

## Technical Context

**Language/Version**: C++23 — GCC 14+ / Clang 18+
**Primary Dependencies**: DuckDB v1.1.3 amalgamation (C API only) via CPMAddPackage — new dependency
**Storage**: DuckDB on-disk database + Parquet epoch archives
**Testing**: Catch2 v3 — unit (`FRE_ENABLE_DUCKDB` guard) + integration
**Target Platform**: Linux server (macOS CI compatible)
**Project Type**: Library extension
**Performance Goals**: CAS < 10ms P99; `query_range()` < 100ms
**Constraints**: No system-installed DuckDB; amalgamation build only; C API only (no C++ API)
**Scale/Scope**: Per-instance embedded store; not a shared/networked database

## Constitution Check

| Gate | Status | Notes |
|------|--------|-------|
| Spec gate | ✅ | spec.md with acceptance scenarios |
| Test gate | ✅ | Unit + integration tests written alongside implementation |
| Dependency gate | ✅ | DuckDB justified: only embedded DB with ACID CAS + Parquet + MIT license + C API |
| Versioning gate | ✅ | MINOR bump — new opt-in capability; no existing API changed |
| Simplicity gate | ✅ | PIMPL hides DuckDB from headers; existing ExternalWindowStore unchanged |
| Resiliency gate | ✅ | Fallback to InProcessWindowStore on any DuckDB failure; `is_degraded()` observable |

## Dependency Justification: DuckDB

| Requirement | DuckDB | chdb | In-house SQLite |
|-------------|--------|------|-----------------|
| Stable C API | ✅ | ❌ Python-first | ✅ |
| CMake amalgamation | ✅ | ❌ | ✅ |
| ACID transactions (CAS) | ✅ | ❌ | ✅ |
| Native Parquet read/write | ✅ | ✅ | ❌ |
| MIT license | ✅ | Apache 2.0 | Public domain |
| Analytical query support | ✅ | ✅ | ❌ poor |

SQLite lacks Parquet and analytical query support. chdb lacks a stable C API and CAS. DuckDB is the only option satisfying all requirements.

## Failure Mode Analysis

| Failure | Blast Radius | Degradation Strategy |
|---------|-------------|---------------------|
| `duckdb_open()` fails | This instance only | `is_available()=false`; ExternalWindowStore falls back to InProcessWindowStore; `is_degraded()=true` |
| WAL corruption on restart | This instance only | DuckDB WAL recovery; if unrecoverable, delete db file → restart in degraded mode |
| Parquet flush fails | Cold-tier data only | Warm tier unaffected; flush retried next interval; operator alerted via log |
| CAS version conflict | Single operation | Returns `false`; caller retries; standard optimistic concurrency |
| Background flush thread panics | Cold-tier archival stops | `std::jthread` destructor joins; warm tier grows but hot path unaffected |

## Three-Tier Architecture

```
Hot tier   (< 1ms):  InProcessWindowStore        — active epoch, in-memory
Warm tier  (< 10ms): DuckDB window_state table   — current + N prior epochs, WAL-backed
Cold tier  (< 100ms): Parquet epoch archives     — immutable epoch snapshots

Recovery:  DuckDB reopens existing DB file → window_state already populated
Fallback:  DuckDB unavailable → ExternalWindowStore degrades to InProcessWindowStore
```

## Schema

```sql
CREATE TABLE IF NOT EXISTS window_state (
    tenant_id    VARCHAR   NOT NULL,
    entity_id    VARCHAR   NOT NULL,
    window_name  VARCHAR   NOT NULL,
    epoch        UBIGINT   NOT NULL,
    aggregate    DOUBLE    NOT NULL DEFAULT 0.0,
    version      UBIGINT   NOT NULL DEFAULT 0,
    updated_at   TIMESTAMP NOT NULL DEFAULT now(),
    PRIMARY KEY (tenant_id, entity_id, window_name, epoch)
);
```

## CAS Pattern

```sql
BEGIN;
INSERT OR IGNORE INTO window_state (tenant_id, entity_id, window_name, epoch, aggregate, version)
    VALUES ($1, $2, $3, $4, 0.0, 0);
UPDATE window_state
    SET aggregate = $5, version = version + 1, updated_at = now()
    WHERE tenant_id=$1 AND entity_id=$2 AND window_name=$3 AND epoch=$4 AND version=$6;
COMMIT;
```
`duckdb_rows_changed() == 0` → version mismatch → return `false`.

## Project Structure

### Documentation (this feature)

```text
specs/004-duckdb-external-storage/
├── plan.md              # This file
├── spec.md              # Feature specification
├── tasks.md             # Task list (all complete)
└── contracts/
    └── duckdb-window-store-contract.md
```

### Source Code

```text
include/fre/state/duckdb_window_store.hpp            # NEW (FRE_ENABLE_DUCKDB guard)
include/fre/evaluator/windowed_historical_evaluator.hpp  # NEW
include/fre/core/error.hpp                           # MODIFIED: QueryRangeError
src/state/duckdb_window_store.cpp                    # NEW
src/pipeline/error.cpp                               # MODIFIED: QueryRangeError message
cmake/dependencies.cmake                             # MODIFIED: DuckDB CPMAddPackage block
CMakeLists.txt                                       # MODIFIED: FRE_ENABLE_DUCKDB option
CMakePresets.json                                    # MODIFIED: duckdb preset
tests/unit/test_duckdb_window_store.cpp              # NEW (FRE_ENABLE_DUCKDB guard)
tests/integration/test_duckdb_backend.cpp            # NEW (FRE_ENABLE_DUCKDB guard)
tests/unit/CMakeLists.txt                            # MODIFIED
tests/integration/CMakeLists.txt                     # MODIFIED
```

## Complexity Tracking

| Decision | Why Needed | Simpler Alternative Rejected Because |
|----------|-----------|-------------------------------------|
| PIMPL for `DuckDbWindowStore` | Keeps `duckdb.h` out of public headers | Exposing DuckDB types in headers forces consumers to have DuckDB installed |
| C API only (no C++ API) | ABI stability across DuckDB versions | C++ API breaks ABI on minor versions; C API is stable |
| Separate read connection for `query_range()` | DuckDB write connection holds write lock during flush | Sharing one connection blocks reads during background flush transactions |
| `std::jthread` for flush | Automatic join on destruction — no manual thread lifecycle | `std::thread` requires explicit join/detach; risky in destructor |
| CPMAddPackage DOWNLOAD_ONLY | Build from amalgamation for zero system dependency | System DuckDB has unpredictable versions and ABI across Linux distros |
