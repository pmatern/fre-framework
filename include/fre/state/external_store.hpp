#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/error.hpp>
#include <fre/core/verdict.hpp>
#include <fre/state/window_store.hpp>

#include <expected>
#include <functional>
#include <memory>

namespace fre {

// ─── ExternalStoreBackend ────────────────────────────────────────────────────

/// Function table for external store backends (Redis adapter, etc.).
/// Backend implementations fill this struct and pass it to ExternalWindowStore.
struct ExternalStoreBackend {
    std::function<std::expected<WindowValue, StoreError>(const WindowKey&)>                     get;
    std::function<std::expected<bool, StoreError>(const WindowKey&, const WindowValue&, const WindowValue&)> compare_and_swap;
    std::function<std::expected<void, StoreError>(const WindowKey&)>                            expire;
    std::function<bool()>                                                                       is_available;
};

// ─── ExternalWindowStore ─────────────────────────────────────────────────────

/// Delegates to an external backend (e.g., Redis) while providing automatic
/// fallback to a local InProcessWindowStore when the backend is unavailable.
///
/// On fallback, sets DegradedReason::StateStoreUnavailable on the calling
/// evaluation path. The fallback store provides local isolation — window state
/// may diverge from the external store until it recovers.
///
/// Satisfies StateStore<ExternalWindowStore>.
class ExternalWindowStore {
public:
    ExternalWindowStore(ExternalStoreBackend backend,
                        std::shared_ptr<InProcessWindowStore> fallback);

    // ─── StateStore concept ───────────────────────────────────────────────────

    [[nodiscard]] std::expected<WindowValue, StoreError> get(const WindowKey& key);

    [[nodiscard]] std::expected<bool, StoreError> compare_and_swap(
        const WindowKey&   key,
        const WindowValue& expected_val,
        const WindowValue& new_val);

    [[nodiscard]] std::expected<void, StoreError> expire(const WindowKey& key);

    [[nodiscard]] bool is_available() const noexcept;

    // ─── Fallback state ───────────────────────────────────────────────────────

    /// True if the last operation used the fallback store.
    [[nodiscard]] bool is_degraded() const noexcept { return using_fallback_; }

private:
    ExternalStoreBackend                  backend_;
    std::shared_ptr<InProcessWindowStore> fallback_;
    mutable bool                          using_fallback_{false};
};

static_assert(StateStore<ExternalWindowStore>);

}  // namespace fre
