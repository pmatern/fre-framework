#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

namespace fre {

/// Typed error returned by Pipeline::submit_sync().
enum class SubmitSyncError : uint8_t {
    Timeout,
    RateLimited,
    PipelineUnavailable,
    NotStarted,
    ValidationFailed,
    Cancelled,
};

// ─── Minimal stop_token / stop_source ────────────────────────────────────────
// Provided because Apple Clang 16 ships without std::stop_token.
// If the standard library has it, use std::stop_token instead.

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
#  include <stop_token>
namespace detail { using StopState = void; }
using StopToken  = std::stop_token;
using StopSource = std::stop_source;
#else

namespace detail {
struct StopState {
    std::atomic<bool> stopped{false};
};
} // namespace detail

/// Minimal stop token (non-owning view of shared stop state).
class StopToken {
public:
    StopToken() noexcept = default;
    explicit StopToken(std::shared_ptr<detail::StopState> state) noexcept
        : state_{std::move(state)} {}

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ && state_->stopped.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool stop_possible() const noexcept {
        return state_ != nullptr;
    }

private:
    std::shared_ptr<detail::StopState> state_;
};

/// Minimal stop source (owns the shared stop state).
class StopSource {
public:
    StopSource() : state_{std::make_shared<detail::StopState>()} {}

    /// Request a stop on all associated tokens.
    void request_stop() noexcept {
        if (state_) {
            state_->stopped.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] StopToken get_token() const noexcept {
        return StopToken{state_};
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ && state_->stopped.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<detail::StopState> state_;
};

#endif // __cpp_lib_jthread

} // namespace fre
