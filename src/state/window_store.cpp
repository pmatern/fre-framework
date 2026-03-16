#include <fre/state/window_store.hpp>

#include <format>

namespace fre {

InProcessWindowStore::InProcessWindowStore(uint32_t num_shards)
    : num_shards_{num_shards}, shards_(num_shards) {}

InProcessWindowStore::~InProcessWindowStore() = default;

std::string InProcessWindowStore::make_key(const WindowKey& k) {
    return std::format("{}:{}:{}:{}", k.tenant_id, k.entity_id, k.window_name, k.epoch);
}

InProcessWindowStore::Shard& InProcessWindowStore::shard_for(const WindowKey& k) noexcept {
    const std::size_t h =
        std::hash<std::string>{}(k.tenant_id) ^ std::hash<std::string>{}(k.entity_id);
    return shards_[h % num_shards_];
}

std::expected<WindowValue, StoreError> InProcessWindowStore::get(const WindowKey& key) {
    auto& shard = shard_for(key);
    std::lock_guard lock{shard.mu};
    const auto key_str = make_key(key);
    if (auto it = shard.data.find(key_str); it != shard.data.end()) {
        return it->second;
    }
    return WindowValue{};  // zero value for missing key
}

std::expected<bool, StoreError> InProcessWindowStore::compare_and_swap(
    const WindowKey&   key,
    const WindowValue& expected_val,
    const WindowValue& new_val) {
    auto& shard = shard_for(key);
    std::lock_guard lock{shard.mu};
    const auto key_str = make_key(key);

    auto it = shard.data.find(key_str);
    const WindowValue current = (it != shard.data.end()) ? it->second : WindowValue{};

    if (current.version != expected_val.version) {
        return false;  // version mismatch — CAS failed
    }

    shard.data[key_str] = new_val;
    return true;
}

std::expected<void, StoreError> InProcessWindowStore::expire(const WindowKey& key) {
    auto& shard = shard_for(key);
    std::lock_guard lock{shard.mu};
    const auto key_str = make_key(key);

    if (auto it = shard.data.find(key_str); it != shard.data.end()) {
        if (expiry_cb_) {
            expiry_cb_(key, it->second);
        }
        shard.data.erase(it);
    }
    return {};
}

void InProcessWindowStore::register_expiry_callback(ExpiryCallback cb) {
    expiry_cb_ = std::move(cb);
}

void InProcessWindowStore::tick(std::chrono::system_clock::time_point /*now*/) {
    // Future: hierarchical time-wheel tick — expire all windows in current slot.
    // For v1: expiry is driven per-evaluator using epoch-based key lookup.
}

}  // namespace fre
