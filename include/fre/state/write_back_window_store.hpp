#pragma once

#ifdef FRE_ENABLE_DUCKDB

/// WriteBackWindowStore — InProcessWindowStore on the hot path, DuckDB in the background.
///
/// Architecture:
///   Hot path (Asio coroutine strand):
///     get() / compare_and_swap() → InProcessWindowStore only (< 1ms always)
///     Successful CAS → inserts key into dirty_set_
///
///   Background jthread (every flush_interval_ms):
///     1. Atomically swaps dirty_set_ out (new CAS calls re-insert freely)
///     2. Reads current value per key from InProcessWindowStore
///     3. Bulk-upserts to DuckDB in a single transaction
///
///   Startup:
///     Constructor calls DuckDbWindowStore::scan_warm_tier() and seeds
///     InProcessWindowStore so counts survive process restarts.
///
/// query_range(): flushes dirty entries synchronously first, then delegates to
/// DuckDB. Acceptable because query_range() is off-hot-path (100ms tolerance).
///
/// is_available(): always true — primary is in-memory and never fails.
/// If DuckDB is unavailable, dirty entries accumulate and retry each cycle.

#include <fre/core/concepts.hpp>
#include <fre/core/error.hpp>
#include <fre/state/duckdb_window_store.hpp>
#include <fre/state/window_store.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fre {

// ─── WriteBackConfig ──────────────────────────────────────────────────────────

struct WriteBackConfig {
    /// How often the background thread flushes dirty entries to DuckDB.
    uint32_t flush_interval_ms{500};
};

// ─── WriteBackWindowStore ─────────────────────────────────────────────────────

class WriteBackWindowStore {
public:
    /// @param primary  In-process hot-path store. Must outlive this object.
    /// @param warm     DuckDB persistence tier. Must outlive this object.
    /// @param config   Flush interval and other settings.
    WriteBackWindowStore(std::shared_ptr<InProcessWindowStore> primary,
                         std::shared_ptr<DuckDbWindowStore>    warm,
                         WriteBackConfig                       config = {});

    ~WriteBackWindowStore();

    WriteBackWindowStore(const WriteBackWindowStore&)            = delete;
    WriteBackWindowStore& operator=(const WriteBackWindowStore&) = delete;
    WriteBackWindowStore(WriteBackWindowStore&&)                 = delete;
    WriteBackWindowStore& operator=(WriteBackWindowStore&&)      = delete;

    // ─── StateStore concept ───────────────────────────────────────────────────

    /// Reads from InProcessWindowStore only. Never touches DuckDB.
    [[nodiscard]] std::expected<WindowValue, StoreError> get(const WindowKey& key);

    /// Writes to InProcessWindowStore only, then marks the key dirty for the
    /// next background flush. Never touches DuckDB on the calling thread.
    [[nodiscard]] std::expected<bool, StoreError> compare_and_swap(
        const WindowKey&   key,
        const WindowValue& expected_val,
        const WindowValue& new_val);

    /// Removes from InProcessWindowStore and schedules a DuckDB deletion on the
    /// next flush cycle.
    [[nodiscard]] std::expected<void, StoreError> expire(const WindowKey& key);

    /// Always true — primary is in-memory and never fails.
    [[nodiscard]] bool is_available() const noexcept { return true; }

    // ─── RangeQueryStore ──────────────────────────────────────────────────────

    /// Flushes all dirty entries synchronously, then delegates to DuckDB.
    /// NOT on the hot path — designed for 100ms-tolerance callers.
    [[nodiscard]] std::expected<double, StoreError> query_range(
        std::string_view tenant_id,
        std::string_view entity_id,
        std::string_view window_name,
        uint64_t         epoch_start,
        uint64_t         epoch_end);

    // ─── Lifecycle ────────────────────────────────────────────────────────────

    /// Flush all pending dirty entries to DuckDB right now (synchronous).
    /// Called automatically from query_range() and from the destructor.
    void flush_sync();

    /// Forward tick() to the primary so expiry callbacks still fire.
    void tick(std::chrono::system_clock::time_point now) { primary_->tick(now); }

    /// Forward expiry callback registration to the primary.
    void register_expiry_callback(InProcessWindowStore::ExpiryCallback cb) {
        primary_->register_expiry_callback(std::move(cb));
    }

private:
    std::shared_ptr<InProcessWindowStore> primary_;
    std::shared_ptr<DuckDbWindowStore>    warm_;
    WriteBackConfig                       config_;

    std::mutex                    dirty_mu_;
    std::unordered_set<WindowKey> dirty_set_;    // keys written since last flush
    std::unordered_set<WindowKey> expired_set_;  // keys expired since last flush

    std::jthread flush_thread_;

    void run_flush_loop(std::stop_token stop);
    void do_flush(std::unordered_set<WindowKey> dirty_snap,
                  std::unordered_set<WindowKey> expired_snap);
    void load_warm_tier();
};

static_assert(StateStore<WriteBackWindowStore>);

}  // namespace fre

#endif  // FRE_ENABLE_DUCKDB
