#ifdef FRE_ENABLE_DUCKDB

#include <fre/state/write_back_window_store.hpp>

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace fre {

// ─── Constructor / Destructor ─────────────────────────────────────────────────

WriteBackWindowStore::WriteBackWindowStore(std::shared_ptr<InProcessWindowStore> primary,
                                            std::shared_ptr<DuckDbWindowStore>    warm,
                                            WriteBackConfig                       config)
    : primary_{std::move(primary)}
    , warm_{std::move(warm)}
    , config_{config}
{
    if (warm_->is_available()) {
        load_warm_tier();
    }

    if (config_.flush_interval_ms > 0) {
        flush_thread_ = std::jthread{[this](std::stop_token stop) {
            run_flush_loop(stop);
        }};
    }
}

WriteBackWindowStore::~WriteBackWindowStore() {
    // Stop the background thread, wait for it to finish its current cycle.
    flush_thread_.request_stop();
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
    // Final drain: persist any entries dirtied after the last background flush.
    flush_sync();
}

// ─── StateStore concept ───────────────────────────────────────────────────────

std::expected<WindowValue, StoreError> WriteBackWindowStore::get(const WindowKey& key) {
    return primary_->get(key);
}

std::expected<bool, StoreError> WriteBackWindowStore::compare_and_swap(
    const WindowKey&   key,
    const WindowValue& expected_val,
    const WindowValue& new_val)
{
    auto result = primary_->compare_and_swap(key, expected_val, new_val);
    if (result.has_value() && *result) {
        std::lock_guard<std::mutex> lock{dirty_mu_};
        dirty_set_.insert(key);
    }
    return result;
}

std::expected<void, StoreError> WriteBackWindowStore::expire(const WindowKey& key) {
    auto result = primary_->expire(key);
    if (result.has_value()) {
        std::lock_guard<std::mutex> lock{dirty_mu_};
        dirty_set_.erase(key);
        expired_set_.insert(key);
    }
    return result;
}

// ─── RangeQueryStore ──────────────────────────────────────────────────────────

std::expected<double, StoreError> WriteBackWindowStore::query_range(
    std::string_view tenant_id,
    std::string_view entity_id,
    std::string_view window_name,
    uint64_t         epoch_start,
    uint64_t         epoch_end)
{
    if (!warm_->is_available()) {
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "DuckDB unavailable"});
    }
    flush_sync();
    return warm_->query_range(tenant_id, entity_id, window_name, epoch_start, epoch_end);
}

// ─── flush_sync ───────────────────────────────────────────────────────────────

void WriteBackWindowStore::flush_sync() {
    std::unordered_set<WindowKey> dirty_snap;
    std::unordered_set<WindowKey> expired_snap;
    {
        std::lock_guard<std::mutex> lock{dirty_mu_};
        // Swap both sets out atomically.
        // New CAS / expire calls after this point will re-insert into the now-empty sets.
        dirty_snap.swap(dirty_set_);
        expired_snap.swap(expired_set_);
    }
    do_flush(std::move(dirty_snap), std::move(expired_snap));
}

// ─── do_flush ────────────────────────────────────────────────────────────────

void WriteBackWindowStore::do_flush(std::unordered_set<WindowKey> dirty_snap,
                                     std::unordered_set<WindowKey> expired_snap)
{
    // ── Upsert dirty entries ──────────────────────────────────────────────────

    if (!dirty_snap.empty() && warm_->is_available()) {
        std::vector<std::pair<WindowKey, WindowValue>> batch;
        batch.reserve(dirty_snap.size());

        for (const auto& key : dirty_snap) {
            // Read the current authoritative value from primary.
            // This may be newer than what originally triggered the dirty mark — that is
            // intentional and correct: we always flush the latest in-memory value.
            auto val = primary_->get(key);
            if (val.has_value()) {
                batch.emplace_back(key, *val);
            }
        }

        auto upsert_result = warm_->upsert_batch(batch);
        if (!upsert_result.has_value()) {
            // DuckDB write failed — re-queue all keys so they are retried next cycle.
            std::lock_guard<std::mutex> lock{dirty_mu_};
            for (const auto& key : dirty_snap) {
                dirty_set_.insert(key);
            }
        }
    } else if (!dirty_snap.empty()) {
        // DuckDB unavailable — re-queue for retry
        std::lock_guard<std::mutex> lock{dirty_mu_};
        for (const auto& key : dirty_snap) {
            dirty_set_.insert(key);
        }
    }

    // ── Delete expired entries ────────────────────────────────────────────────

    for (const auto& key : expired_snap) {
        // Ignore failures: if DuckDB is down the row will be overwritten on recovery
        // (upsert_batch uses version from in-memory which has moved past the expired key).
        (void)warm_->expire(key);
    }
}

// ─── Background flush loop ───────────────────────────────────────────────────

void WriteBackWindowStore::run_flush_loop(std::stop_token stop) {
    const auto interval = std::chrono::milliseconds{config_.flush_interval_ms};
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(interval);
        if (stop.stop_requested()) break;
        flush_sync();
    }
}

// ─── Startup recovery ────────────────────────────────────────────────────────

void WriteBackWindowStore::load_warm_tier() {
    auto rows = warm_->scan_warm_tier();
    if (!rows.has_value()) return;

    for (auto& [key, val] : *rows) {
        // In-memory is empty at startup; CAS from {0,0} seeds the value.
        // If the key somehow already exists (shouldn't happen on fresh start),
        // this is a no-op (CAS returns false but that's fine).
        (void)primary_->compare_and_swap(key, WindowValue{}, val);
        // Deliberately NOT adding to dirty_set_: these reads from DuckDB
        // are already persisted — no need to write them back.
    }
}

}  // namespace fre

#endif  // FRE_ENABLE_DUCKDB
