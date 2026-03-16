#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fre {

// ─── Tag ─────────────────────────────────────────────────────────────────────

struct Tag {
    std::string_view key;
    std::string_view value;
};

// ─── Event ───────────────────────────────────────────────────────────────────

struct Event {
    /// Monotonically increasing identifier assigned by the ingest stage.
    uint64_t id{0};

    /// Tenant identifier — used for sharding and rate limiting.
    std::string tenant_id;

    /// Entity being evaluated (user, device, session, etc.).
    std::string entity_id;

    /// Semantic type of the event (e.g., "api_call", "login_attempt").
    std::string event_type;

    /// Wall-clock time at which the event occurred.
    std::chrono::system_clock::time_point timestamp;

    /// Raw payload bytes. Lifetime managed by the caller; must outlive the Event.
    std::span<const std::byte> payload;

    /// Metadata key-value pairs. Lifetime managed by the caller.
    std::span<const Tag> tags;

    /// Returns true if tenant_id and entity_id are non-empty.
    [[nodiscard]] bool is_valid() const noexcept {
        return !tenant_id.empty() && !entity_id.empty();
    }

    /// Retrieve a tag value by key; returns empty string_view if absent.
    [[nodiscard]] std::string_view tag(std::string_view key) const noexcept {
        for (const auto& t : tags) {
            if (t.key == key) return t.value;
        }
        return {};
    }
};

}  // namespace fre
