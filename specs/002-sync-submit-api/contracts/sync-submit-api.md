# Contract: Synchronous Blocking Submit API

**Feature**: 002-sync-submit-api
**Type**: C++ Public Library API
**Date**: 2026-03-15

---

## New Header: `include/fre/pipeline/sync_submit.hpp`

```cpp
#pragma once
#include <cstdint>

namespace fre {

/// Typed error returned by Pipeline::submit_sync().
/// Each variant is mutually exclusive and unambiguous.
enum class SubmitSyncError : uint8_t {
    /// Decision not produced within the pipeline's latency budget.
    Timeout,

    /// Rate-limit token bucket or concurrency cap exhausted.
    RateLimited,

    /// Pipeline is in Draining or Stopped state (was running but is now unavailable).
    PipelineUnavailable,

    /// Pipeline has never been started.
    NotStarted,

    /// Event failed ingest-stage validation.
    ValidationFailed,

    /// Caller requested cancellation via std::stop_token before a decision arrived.
    Cancelled,
};

} // namespace fre
```

---

## Modified: `include/fre/pipeline/pipeline.hpp`

Add to `class Pipeline`:

```cpp
#include <stop_token>              // std::stop_token
#include <expected>                // std::expected
#include <fre/pipeline/sync_submit.hpp>
#include <fre/core/decision.hpp>

/// Submit an event and block until the pipeline produces a Decision.
///
/// Behaviour:
///  - Returns the Decision on success. Registered emission targets also fire (same
///    as async submit) before the call returns.
///  - Returns SubmitSyncError::Timeout if no decision is produced within the
///    pipeline's configured latency budget.
///  - Returns SubmitSyncError::RateLimited if the rate-limit or concurrency cap is
///    exhausted at submission time.
///  - Returns SubmitSyncError::PipelineUnavailable if the pipeline is Draining/Stopped.
///  - Returns SubmitSyncError::NotStarted if the pipeline has never been started.
///  - Returns SubmitSyncError::ValidationFailed if the event fails ingest validation.
///  - Returns SubmitSyncError::Cancelled if `cancel` is requested before a decision
///    arrives; no decision is emitted to targets in this case.
///
/// Thread safety: safe to call concurrently from multiple threads.
/// The call NEVER blocks longer than the pipeline's latency budget regardless of cancel.
///
/// @param event   The event to evaluate. Copied into the pipeline (caller retains ownership).
/// @param cancel  Optional cancellation token. Default {} means no cancellation.
/// @return        Decision on success, SubmitSyncError on any failure.
[[nodiscard]] std::expected<Decision, SubmitSyncError>
submit_sync(Event event, std::stop_token cancel = {});
```

---

## Invariants

| # | Invariant |
|---|---|
| INV-1 | If `submit_sync` returns a `Decision`, that same decision has already been delivered to all registered `EmissionFn` targets. |
| INV-2 | If `submit_sync` returns any `SubmitSyncError`, zero decisions are emitted to emission targets for that event. |
| INV-3 | The wall-clock time from call entry to return is ≤ `PipelineConfig::latency_budget + ε` (where ε is OS scheduling jitter, bounded to a small multiple of one thread-wake latency). |
| INV-4 | `submit_sync` and `submit` on the same `Pipeline` instance do not interfere: each call's Decision is routed only to its own caller (and emission targets). |
| INV-5 | Setting `cancel` before calling `submit_sync` results in `Cancelled` immediately, without submitting the event. |
| INV-6 | Setting `cancel` after the Decision has been produced results in the Decision being returned normally (cancellation is a no-op). |

---

## Concept Satisfaction

The new method does not introduce a new concept. `submit_sync` is a concrete method on `Pipeline`. No template or concept change is required.

---

## Backward Compatibility

- `submit_sync` is a new method — existing code compiles without modification (MINOR version bump).
- The `std::stop_token cancel = {}` default argument makes the parameter entirely optional.
- `SubmitSyncError` is a new type in `namespace fre` — no existing names are shadowed.
- `sync_submit.hpp` is a new header — existing `#include` chains are unaffected.
