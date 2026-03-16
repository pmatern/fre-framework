#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/error.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fre {

// ─── AggregationFn ──────────────────────────────────────────────────────────

enum class AggregationFn : uint8_t {
    Count,
    Sum,
    DistinctCount,
};

// ─── GroupBy ─────────────────────────────────────────────────────────────────

enum class GroupBy : uint8_t {
    EntityId,
    TenantId,
    Tag,
};

// ─── WindowType ──────────────────────────────────────────────────────────────

enum class WindowType : uint8_t {
    Tumbling,
    Sliding,
    Session,
};

// ─── WindowKey ───────────────────────────────────────────────────────────────

struct WindowKey {
    std::string    tenant_id;
    std::string    entity_id;
    std::string    window_name;  // identifies the evaluator configuration
    uint64_t       epoch{0};     // tumbling: floor(timestamp_ms / window_ms)

    bool operator==(const WindowKey& other) const noexcept {
        return tenant_id == other.tenant_id &&
               entity_id == other.entity_id &&
               window_name == other.window_name &&
               epoch == other.epoch;
    }
};

// ─── WindowValue ─────────────────────────────────────────────────────────────

struct WindowValue {
    double   aggregate{0.0};   // running count/sum/distinct-count
    uint64_t version{0};       // optimistic concurrency version; incremented on CAS success

    // For DistinctCount: would hold a sketch/set (simplified for v1 — use count)
};

// ─── InProcessWindowStore ────────────────────────────────────────────────────

/// Default in-process windowed state store.
/// Ring-buffer indexed by epoch for tumbling windows.
/// Per-shard mutex for concurrent access safety on the hot path.
class InProcessWindowStore {
public:
    explicit InProcessWindowStore(uint32_t num_shards = 64);
    ~InProcessWindowStore();

    // ─── StateStore concept ───────────────────────────────────────────────────

    [[nodiscard]] std::expected<WindowValue, StoreError> get(const WindowKey& key);

    [[nodiscard]] std::expected<bool, StoreError> compare_and_swap(
        const WindowKey&   key,
        const WindowValue& expected_val,
        const WindowValue& new_val);

    [[nodiscard]] std::expected<void, StoreError> expire(const WindowKey& key);

    [[nodiscard]] bool is_available() const noexcept { return true; }

    // ─── Expiry ───────────────────────────────────────────────────────────────

    /// Register a callback to be called when a window epoch expires.
    using ExpiryCallback = std::function<void(const WindowKey&, const WindowValue&)>;
    void register_expiry_callback(ExpiryCallback cb);

    /// Advance the time wheel to `now`, expiring all windows in elapsed ticks.
    /// Called periodically by a background timer.
    void tick(std::chrono::system_clock::time_point now);

private:
    struct Shard {
        std::mutex                                          mu;
        std::unordered_map<std::string, WindowValue>        data;  // key_str -> value
    };

    uint32_t              num_shards_;
    std::vector<Shard>    shards_;
    ExpiryCallback        expiry_cb_;

    [[nodiscard]] static std::string make_key(const WindowKey& k);
    [[nodiscard]] Shard& shard_for(const WindowKey& k) noexcept;
};

static_assert(StateStore<InProcessWindowStore>);

}  // namespace fre

// ─── std::hash specialization for WindowKey ──────────────────────────────────

template <>
struct std::hash<fre::WindowKey> {
    std::size_t operator()(const fre::WindowKey& k) const noexcept {
        std::size_t h = 0;
        auto combine  = [&](std::size_t v) { h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2); };
        combine(std::hash<std::string>{}(k.tenant_id));
        combine(std::hash<std::string>{}(k.entity_id));
        combine(std::hash<std::string>{}(k.window_name));
        combine(std::hash<uint64_t>{}(k.epoch));
        return h;
    }
};
