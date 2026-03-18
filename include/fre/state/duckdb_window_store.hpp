#pragma once

#ifdef FRE_ENABLE_DUCKDB

/// DuckDB-backed StateStore implementation.
///
/// Three-tier storage model:
///   Hot  (< 1ms):  InProcessWindowStore (existing, always present)
///   Warm (< 10ms): DuckDB window_state table — current + N prior epochs, WAL-backed
///   Cold (OLAP):   Parquet archive — immutable epoch snapshots in parquet_archive_dir
///
/// Usage:
///   auto store  = DuckDbWindowStore{DuckDbConfig{.db_path = "/data/fre/state.duckdb"}};
///   auto backend = store.as_backend();           // fills ExternalStoreBackend vtable
///   auto ext    = ExternalWindowStore{backend, fallback_store};  // wrap with fallback
///
/// Recovery: reopening the same db_path restores all in-table window state automatically.
/// Archival: completed epochs are exported to parquet and deleted from the warm tier
///           by a background std::jthread running every flush_interval_ms.
///
/// Thread safety: get/compare_and_swap/expire are protected by an internal mutex and
///                safe to call concurrently from multiple asio strands.

#include <fre/core/error.hpp>
#include <fre/state/external_store.hpp>
#include <fre/state/window_store.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace fre {

// ─── DuckDbConfig ─────────────────────────────────────────────────────────────

struct DuckDbConfig {
    /// Path to the DuckDB database file.
    /// Empty string = in-memory database (no persistence, useful for tests).
    std::string db_path;

    /// Directory for parquet epoch archives.
    /// Empty string = archival disabled (warm tier only, no cold tier).
    /// Layout: <parquet_archive_dir>/epoch=<N>/part-0000.parquet
    std::string parquet_archive_dir;

    /// How often the background flush thread exports old epochs to parquet.
    uint32_t flush_interval_ms{60000};

    /// Window duration in milliseconds — used to compute epoch boundaries.
    uint64_t window_ms{60000};

    /// Number of recent epochs to retain in the warm (DuckDB) tier.
    /// Epochs older than (current_epoch - warm_epoch_retention) are flushed.
    uint32_t warm_epoch_retention{3};
};

// ─── DuckDbWindowStore ────────────────────────────────────────────────────────

class DuckDbWindowStore {
public:
    explicit DuckDbWindowStore(DuckDbConfig config);
    ~DuckDbWindowStore();

    DuckDbWindowStore(DuckDbWindowStore&&) noexcept;
    DuckDbWindowStore& operator=(DuckDbWindowStore&&) noexcept;

    DuckDbWindowStore(const DuckDbWindowStore&)            = delete;
    DuckDbWindowStore& operator=(const DuckDbWindowStore&) = delete;

    // ─── StateStore concept ───────────────────────────────────────────────────

    /// Returns WindowValue{0.0, 0} if the key does not exist (matches InProcessWindowStore).
    [[nodiscard]] std::expected<WindowValue, StoreError> get(const WindowKey& key);

    /// Optimistic CAS: updates aggregate and increments version if current version
    /// matches expected_val.version.  Returns true on success, false on version mismatch.
    [[nodiscard]] std::expected<bool, StoreError> compare_and_swap(
        const WindowKey&   key,
        const WindowValue& expected_val,
        const WindowValue& new_val);

    [[nodiscard]] std::expected<void, StoreError> expire(const WindowKey& key);

    /// Returns false if the database could not be opened at construction time.
    [[nodiscard]] bool is_available() const noexcept;

    // ─── Fleet-level utilities ────────────────────────────────────────────────

    /// Fill an ExternalStoreBackend vtable whose lambdas delegate to this store.
    /// The returned backend holds a raw pointer to this store — the caller must
    /// ensure this DuckDbWindowStore outlives the backend.
    [[nodiscard]] ExternalStoreBackend as_backend();

    // ─── Long-horizon analytical query ────────────────────────────────────────

    /// Sum aggregate values across [epoch_start, epoch_end] inclusive,
    /// querying both the warm (DuckDB) tier and the cold (parquet) tier.
    ///
    /// This is NOT on the hot path — call from a background coroutine or thread.
    /// Uses a dedicated read connection that does not block the hot-path connection.
    [[nodiscard]] std::expected<double, StoreError> query_range(
        std::string_view tenant_id,
        std::string_view entity_id,
        std::string_view window_name,
        uint64_t         epoch_start,
        uint64_t         epoch_end);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

static_assert(StateStore<DuckDbWindowStore>);

}  // namespace fre

#endif  // FRE_ENABLE_DUCKDB
