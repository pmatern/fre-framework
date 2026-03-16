# Contract: Pluggable State Store

**Version**: 1.0.0

## Overview

The `StateStore` concept defines the interface between the pipeline's windowed aggregation
engine and any backing store for window state. The in-process time-wheel implementation
satisfies this concept and is used by default. External backends (Redis, Memcached, custom)
satisfy the same concept and are swapped in via `PipelineConfig::state_store`.

The interface is intentionally minimal — it exposes only the operations needed for
compare-and-swap windowed aggregation, not a general key-value store.

---

## Concept: `StateStore<S>`

```
concept StateStore<S> requires:
  S::get(WindowKey) -> expected<std::optional<WindowValue>, StoreError>
  S::compare_and_swap(WindowKey, std::optional<WindowValue> expected,
                      WindowValue desired,
                      std::chrono::milliseconds ttl)
                   -> expected<bool, StoreError>
  S::expire(WindowKey) -> expected<void, StoreError>
  S::is_available() -> bool
```

### `get`

Returns the current `WindowValue` for a key, or `nullopt` if no value exists for that key.

- MUST be non-blocking in the in-process implementation.
- For external backends, MUST return within the stage's remaining latency budget.
- On error, returns `StoreError`.

### `compare_and_swap`

Atomically update the value for a key only if the current value matches `expected`.
Sets the entry TTL to `ttl` on success (resets on each successful update).

- Returns `true` if the swap succeeded (value matched and was updated).
- Returns `false` if the value did not match `expected` (caller should retry with fresh `get`).
- Returns `StoreError` on backend failure.
- `ttl` MUST be ≥ the configured window duration to prevent premature expiry.

### `expire`

Immediately removes the entry for a key, regardless of TTL.

Called by the pipeline when a window closes at its natural boundary.

### `is_available`

Non-blocking health probe. Returns `false` if the store backend is known to be unreachable.

Called by the pipeline before each window operation. On `false`, the pipeline falls back to
in-process state and sets `DegradedReason::StateStoreUnavailable`.

---

## WindowKey

Composite key uniquely identifying one window accumulator.

| Field | Type | Description |
|-------|------|-------------|
| `tenant_id` | `string_view` | Owning tenant |
| `entity_id` | `string_view` | Entity being tracked |
| `rule_id` | `string_view` | Which threshold rule owns this window |
| `window_epoch` | `int64_t` | Epoch index (floor(timestamp / window_duration)) for tumbling windows; 0 for sliding |

Serialized form for external backends: `{tenant_id}:{entity_id}:{rule_id}:{window_epoch}`

---

## WindowValue

| Field | Type | Description |
|-------|------|-------------|
| `aggregate` | `double` | Current aggregated value (count, sum, or distinct-count HLL sketch) |
| `event_count` | `uint64_t` | Raw event count in this window (for audit) |
| `window_start` | `int64_t` | Unix timestamp (ms) of window start |
| `version` | `uint64_t` | Optimistic concurrency version; incremented on each CAS success |

---

## StoreError enumeration

| Variant | Description | Framework action |
|---------|-------------|-----------------|
| `Timeout` | Backend did not respond in time | Fall back to in-process; set `StateStoreUnavailable` |
| `ConnectionLost` | Network connection to backend failed | Fall back to in-process; set `StateStoreUnavailable` |
| `Serialization` | Value could not be deserialized | Log error; treat window as empty (reset) |
| `Conflict` | CAS failed due to concurrent update | Caller retries (framework retries up to 3 times) |
| `Capacity` | Backend rejected write due to capacity | Fall back to in-process; set `StateStoreUnavailable` |

---

## In-Process Default: Time-Wheel Implementation

The default `InProcessWindowStore` uses a hierarchical time-wheel:

- 256 slots, each representing `max_window_duration / 256` of time
- Each slot holds a hash map of `WindowKey → WindowValue`
- Advancing the wheel is O(1) amortised; expiry is O(k) for k windows expiring in that slot
- Per-shard mutex (one per worker cell) limits lock contention to the shard's tenants
- No allocation on the hot path: pre-allocated slab allocator per shard

---

## Redis Adapter (Reference Implementation)

The optional `RedisWindowStore` satisfies `StateStore` using:

- `GET {key}` → `get()`
- `SET {key} {value} PX {ttl_ms} XX` for CAS (with optimistic locking via `version` field)
- `DEL {key}` → `expire()`
- `PING` (non-blocking, cached result for 100ms) → `is_available()`

Serialization: MessagePack for `WindowValue` (compact binary, no schema dependency).

The Redis adapter is provided as a separate optional target in CMake and is NOT a dependency
of the core library.
