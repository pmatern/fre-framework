# Implementation Plan: Write-Back Window Store

**Branch**: `006-write-back-window-store` | **Date**: 2026-03-28 | **Spec**: [spec.md](spec.md)

## Architecture

```
Hot path (Asio coroutine strand):
  get() / compare_and_swap() → InProcessWindowStore only (< 1ms)
  Successful CAS → insert key into dirty_set_ (brief dirty_mu_ lock)

Background jthread (every flush_interval_ms):
  1. swap(dirty_set_, empty)     — atomic snapshot, new CAS calls re-insert freely
  2. read current value per key from InProcessWindowStore
  3. DuckDbWindowStore::upsert_batch() — single BEGIN / N×INSERT ON CONFLICT / COMMIT

Startup:
  DuckDbWindowStore::scan_warm_tier() → seed InProcessWindowStore
  dirty_set_ stays empty (reads-in, not new writes)
```

## Key Design Decisions

- **Dirty set is a single `std::unordered_set<WindowKey>`** under one `dirty_mu_`. Critical
  section is a hash-set insert (~nanoseconds). Not sharded — no measurable benefit at this size.
- **Flush reads current value, not value at dirty-mark time.** If a key is CAS'd again between
  snapshot and read, we flush the newer value. The re-dirtied key appears in the next flush cycle.
  No data is lost.
- **DuckDB write is unconditional `INSERT ON CONFLICT DO UPDATE`** — no version guard. Primary
  is authoritative; version in DuckDB follows whatever the primary holds at flush time.
- **`expire()` moves keys to `expired_set_`** (also under `dirty_mu_`). Flush loop calls
  `warm_->expire(key)` for each. Erase from `dirty_set_` at expire time to avoid a redundant upsert.
- **Destructor drain**: `request_stop()` + `join()` the jthread, then one final `flush_sync()`.
- **`query_range()` calls `flush_sync()` first**, ensuring DuckDB sees all dirty in-memory writes
  before the historical query runs.

## Files

| File | Role |
|---|---|
| `include/fre/state/write_back_window_store.hpp` | Class declaration |
| `src/state/write_back_window_store.cpp` | Implementation |
| `include/fre/state/duckdb_window_store.hpp` | `upsert_batch()`, `scan_warm_tier()` declarations |
| `src/state/duckdb_window_store.cpp` | `upsert_batch()`, `scan_warm_tier()` implementations |
| `include/fre/evaluator/threshold_evaluator.hpp` | Templatized; impl moved inline |
| `src/evaluator/threshold_evaluator.cpp` | Stub (implementation in header) |
| `CMakeLists.txt` | Add source; fix BUILD_INTERFACE wrapper for duckdb |
| `include/fre/evaluator/windowed_historical_evaluator.hpp` | Activate static_asserts |
