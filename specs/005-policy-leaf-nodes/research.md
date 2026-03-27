# Research: Extended Policy Rule Leaf Nodes

## Confirmed Type Shapes

The following fields from existing headers are confirmed and referenced by the new leaf nodes:

### Event fields (`include/fre/core/event.hpp`)
| Field | Type | Used by |
|---|---|---|
| `event_type` | `std::string` | `EventTypeIs`, `EventTypeIn` |
| `tenant_id` | `std::string` | `TenantIs` |
| `timestamp` | `std::chrono::system_clock::time_point` | `EventOlderThan`, `EventNewerThan` |
| `tags` | `std::span<const Tag>` | all `Tag*` nodes |
| `Tag::key` | `std::string_view` | all `Tag*` nodes |
| `Tag::value` | `std::string_view` | all `Tag*` nodes |

### EvaluatorResult fields (`include/fre/core/verdict.hpp`)
| Field | Type | Used by |
|---|---|---|
| `evaluator_id` | `std::string` | all `Evaluator*` nodes |
| `score` | `std::optional<float>` | `EvaluatorScoreBetween` |
| `skipped` | `bool` | `EvaluatorWasSkipped` |
| `reason_code` | `std::optional<std::string>` | `EvaluatorReasonIs` |

### StageOutput fields (`include/fre/core/verdict.hpp`)
| Field | Type | Used by |
|---|---|---|
| `stage_id` | `std::string` | `StageIsDegraded` |
| `degraded_reason` | `DegradedReason` (uint16_t bitmask) | `StageIsDegraded` |
| `evaluator_results` | `std::vector<EvaluatorResult>` | all `Evaluator*` nodes |

`is_degraded(DegradedReason)` is a free function returning `r != DegradedReason::None`.

---

## Decision: Numeric Tag Parsing

**Decision**: Use `std::from_chars` (C++17, available in C++23) to parse `double` from `string_view`.

**Rationale**:
- Does not throw; returns `std::from_chars_result` with `ec` error code.
- Works directly on `const char*` range derived from `string_view::data()` + `size()`.
- Consistent with the existing codebase rule: no exceptions.
- Faster than `std::stod` and avoids locale dependency.

**Alternatives considered**:
- `std::stod` — throws `std::invalid_argument` / `std::out_of_range`; prohibited.
- `sscanf` — no C-string guarantee from `string_view`; unsafe without null termination.

**Implementation pattern**:
```cpp
double val{};
auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
if (ec != std::errc{}) return false;   // absent or non-numeric → no match
```

---

## Decision: TagIn Storage Type

**Decision**: Store allowed values as `std::vector<std::string>` in the `TagIn` node.

**Rationale**:
- Rule sets typically contain small value lists (< 20 entries); linear scan is negligible.
- `std::vector` has value semantics and is trivially copyable with the existing deep-copy pattern.
- `std::unordered_set` would require a custom hash for `string_view` lookup against `std::string` keys; not worth the complexity for this use case.

**Alternatives considered**:
- `std::unordered_set<std::string>` — O(1) lookup but higher complexity, no benefit for small N.
- `std::set<std::string>` — O(log N) lookup, copyable, but still unnecessary overhead.

---

## Decision: EventOlderThan / EventNewerThan Timestamp Comparison

**Decision**: Use `std::chrono::system_clock::now()` at evaluation time (inside `RuleEngine::evaluate`).

**Rationale**:
- Consistent with how the ingest stage already computes clock skew (`now - event.timestamp`).
- Future-timestamped events produce a negative duration; treat as non-matching (duration cast to unsigned would wrap; use signed comparison: `auto age = now - event.timestamp; if (age < duration::zero()) return false;`).

**Alternatives considered**:
- Passing evaluation time via `PolicyContext` — would require a context API change; unnecessary for this feature.

---

## Decision: TagValueBetween / EvaluatorScoreBetween Bounds Semantics

**Decision**: `[lo, hi)` — inclusive lower bound, exclusive upper bound.

**Rationale**:
- Consistent with C++ range conventions (`[begin, end)`).
- Allows adjacent ranges to tile cleanly: `[0.0, 0.5)` and `[0.5, 1.0)` are non-overlapping.
- Matches the spec acceptance scenario: `score=0.4` with `Between{0.4, 0.8}` must match.

---

## Constitution Gate Pre-Check

| Gate | Status | Notes |
|---|---|---|
| Spec gate | ✅ PASS | `spec.md` complete, all acceptance scenarios present |
| Test gate | ✅ PASS | TDD plan: tests written before implementation |
| Dependency gate | ✅ PASS | `std::from_chars` is stdlib (C++17); zero new external deps |
| Versioning gate | ✅ PASS | MINOR bump — new types added to `PolicyRule::Variant`; no existing API removed or changed |
| Simplicity gate | ✅ PASS | Pure extension of existing `if constexpr` chain; no new abstractions |
| Resiliency gate | ✅ PASS | See failure mode analysis in plan.md; rule eval is CPU-only, latency impact negligible |
