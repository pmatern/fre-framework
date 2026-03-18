# Contract: DuckDbWindowStore

**Feature**: 004-duckdb-external-storage
**File**: `include/fre/state/duckdb_window_store.hpp`
**Guard**: `#ifdef FRE_ENABLE_DUCKDB`

## Interface

```cpp
struct DuckDbConfig {
    std::string db_path;             // "" = in-memory (no restart recovery)
    std::string parquet_archive_dir; // "" = no archival (warm tier only)
    uint32_t flush_interval_ms{60000};    // 0 = disable background flush
    uint64_t window_ms{60000};            // epoch duration in ms
    uint32_t warm_epoch_retention{3};     // epochs to keep in DuckDB before archiving
};

class DuckDbWindowStore {
public:
    explicit DuckDbWindowStore(DuckDbConfig config);
    ~DuckDbWindowStore();  // joins flush thread

    // StateStore concept interface
    [[nodiscard]] std::expected<WindowValue, StoreError>
        get(const WindowKey& key);

    [[nodiscard]] std::expected<bool, StoreError>
        compare_and_swap(const WindowKey& key,
                         const WindowValue& expected,
                         const WindowValue& desired);

    [[nodiscard]] std::expected<void, StoreError>
        expire(const WindowKey& key);

    [[nodiscard]] bool is_available() const noexcept;

    // ExternalWindowStore integration
    [[nodiscard]] ExternalStoreBackend as_backend();

    // Long-horizon queries — NOT on hot path (<100ms, not <10ms)
    [[nodiscard]] std::expected<double, StoreError>
        query_range(std::string_view tenant_id,
                    std::string_view entity_id,
                    std::string_view window_name,
                    uint64_t epoch_start,
                    uint64_t epoch_end);
};

static_assert(StateStore<DuckDbWindowStore>);
```

## Invariants

1. `is_available()` returns false iff `duckdb_open()` failed during construction; never throws.
2. `compare_and_swap()` is atomic: uses a `BEGIN/INSERT OR IGNORE/UPDATE WHERE version=?/COMMIT` transaction; returns `false` on version mismatch, `true` on success.
3. `get()` on a missing key returns `{aggregate: 0.0, version: 0}` — identical to `InProcessWindowStore`.
4. `query_range()` uses a separate DuckDB connection (protected by `query_mutex_`) — never blocks the CAS path.
5. The background flush `std::jthread` is joined in the destructor — no dangling thread.
6. All DuckDB C API calls use RAII wrappers; no raw `duckdb_*` handles escape the `Impl`.

## StateStore Concept Mapping

| Concept Method | DuckDB Implementation |
|---------------|----------------------|
| `get(key)` | Prepared SELECT on `window_state` |
| `compare_and_swap(key, old, new)` | BEGIN/INSERT OR IGNORE/UPDATE WHERE version=old.version/COMMIT |
| `expire(key)` | Prepared DELETE on `window_state` |
| `is_available()` | `impl_->db_ != nullptr` |

## ExternalStoreBackend Vtable

`as_backend()` returns an `ExternalStoreBackend` with lambdas capturing `this`:
- `get_fn`: calls `this->get(key)`
- `cas_fn`: calls `this->compare_and_swap(key, old, new)`
- `expire_fn`: calls `this->expire(key)`
- `available_fn`: calls `this->is_available()`

This allows `ExternalWindowStore` to use DuckDB as a drop-in backend with its existing fallback logic.

## RangeQueryStore Concept (for WindowedHistoricalEvaluator)

```cpp
template <typename S>
concept RangeQueryStore = requires(S s, std::string_view sv, uint64_t u) {
    { s.query_range(sv, sv, sv, u, u) } -> std::same_as<std::expected<double, StoreError>>;
};
```

`DuckDbWindowStore` satisfies `RangeQueryStore`.

## CMake Integration

```cmake
# Only available when FRE_ENABLE_DUCKDB=ON
cmake --preset duckdb        # configure
cmake --build --preset duckdb
ctest --preset duckdb        # runs duckdb-guarded tests
```
