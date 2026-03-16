#include <fre/state/external_store.hpp>

namespace fre {

ExternalWindowStore::ExternalWindowStore(ExternalStoreBackend backend,
                                          std::shared_ptr<InProcessWindowStore> fallback)
    : backend_{std::move(backend)}, fallback_{std::move(fallback)} {}

bool ExternalWindowStore::is_available() const noexcept {
    if (!backend_.is_available) return false;
    return backend_.is_available();
}

std::expected<WindowValue, StoreError> ExternalWindowStore::get(const WindowKey& key) {
    if (is_available()) {
        using_fallback_ = false;
        return backend_.get(key);
    }
    using_fallback_ = true;
    return fallback_->get(key);
}

std::expected<bool, StoreError> ExternalWindowStore::compare_and_swap(
    const WindowKey&   key,
    const WindowValue& expected_val,
    const WindowValue& new_val) {
    if (is_available()) {
        using_fallback_ = false;
        return backend_.compare_and_swap(key, expected_val, new_val);
    }
    using_fallback_ = true;
    return fallback_->compare_and_swap(key, expected_val, new_val);
}

std::expected<void, StoreError> ExternalWindowStore::expire(const WindowKey& key) {
    if (is_available()) {
        using_fallback_ = false;
        return backend_.expire(key);
    }
    using_fallback_ = true;
    return fallback_->expire(key);
}

}  // namespace fre
