# Data Model: Extended Policy Rule Leaf Nodes

## Overview

All 13 new types are **leaf nodes** added to the `PolicyRule::Variant`. They are pure value types
with no heap-allocated children (unlike `And`/`Or`/`Not`). All are fully copyable by default.

The existing `PolicyRule::Variant` currently holds:
`StageVerdictIs | EvaluatorScoreAbove | TagEquals | And | Or | Not`

After this feature it holds all of the above plus the 13 new types below.

---

## New Leaf Node Types

### Group 1: Tag String Matching

```
TagContains
  key        : std::string   — tag key to look up on the event
  substring  : std::string   — value must contain this (case-sensitive)

TagStartsWith
  key        : std::string   — tag key to look up on the event
  prefix     : std::string   — value must begin with this

TagIn
  key        : std::string          — tag key to look up on the event
  values     : std::vector<string>  — value must be an exact member of this set

TagExists
  key        : std::string   — tag key; matches if present regardless of value
```

**Evaluation source**: `PolicyContext::event.tags` (scanned linearly by key).

---

### Group 2: Numeric Tag Comparisons

```
TagValueLessThan
  key        : std::string   — tag key
  threshold  : double        — tag value parsed as double must be < threshold

TagValueGreaterThan
  key        : std::string   — tag key
  threshold  : double        — tag value parsed as double must be > threshold

TagValueBetween
  key        : std::string   — tag key
  lo         : double        — lower bound (inclusive)
  hi         : double        — upper bound (exclusive)
```

**Parsing**: `std::from_chars` on `Tag::value` (a `std::string_view`). Returns false on absent
tag or parse failure — never throws.

**Evaluation source**: `PolicyContext::event.tags`.

---

### Group 3: Event Field Matching

```
EventTypeIs
  event_type : std::string   — exact match against Event::event_type

EventTypeIn
  event_types : std::vector<string>  — membership match against Event::event_type

TenantIs
  tenant_id  : std::string   — exact match against Event::tenant_id

EventOlderThan
  duration   : std::chrono::milliseconds   — now - event.timestamp > duration
               Note: callers pass seconds via duration_cast or ms literals (e.g.
               std::chrono::seconds{30} is implicitly convertible to milliseconds)

EventNewerThan
  duration   : std::chrono::milliseconds   — now - event.timestamp < duration
               Same note as above.
```

**Evaluation source**: `PolicyContext::event` fields directly.
**Duration type rationale**: `std::chrono::milliseconds` is used for consistency with
`PolicyStageConfig::timeout`, `IngestStageConfig::skew_tolerance`, and other duration
fields in the codebase. `std::chrono::seconds` literals (`30s`) implicitly convert to
`milliseconds` at the call site — no cast required.
**Timestamp note**: Future-timestamped events yield a negative age; treated as non-matching.

---

### Group 4: Evaluator Score Range and Pipeline Health

```
EvaluatorScoreBetween
  evaluator_id : std::string   — matches EvaluatorResult::evaluator_id
  lo           : float         — lower bound (inclusive)
  hi           : float         — upper bound (exclusive)

StageIsDegraded
  stage_id     : std::string   — matches StageOutput::stage_id
                                 true if stage's degraded_reason != DegradedReason::None

EvaluatorWasSkipped
  evaluator_id : std::string   — matches EvaluatorResult::evaluator_id
                                 true if result.skipped == true

EvaluatorReasonIs
  evaluator_id : std::string   — matches EvaluatorResult::evaluator_id
  reason_code  : std::string   — matches EvaluatorResult::reason_code (optional<string>)
```

**Evaluation source**: `PolicyContext::stage_outputs` (scanned linearly by stage_id /
evaluator_id).

---

## Unchanged Structures

- `PolicyContext` — no changes; existing `event` and `stage_outputs` fields are sufficient.
- `StageOutput` — no changes.
- `EvaluatorResult` — no changes.
- `PolicyStageConfig` / `PolicyStageRule` — no changes; `add_rule` accepts any `PolicyRule`.
- `PolicyStage::process` — no changes; delegates entirely to `RuleEngine::evaluate`.

---

## Variant Expansion Summary

| Before | After |
|---|---|
| 6 node types | 19 node types |
| `std::variant<StageVerdictIs, EvaluatorScoreAbove, TagEquals, And, Or, Not>` | + 13 new leaf types |

The `std::visit` dispatcher in `RuleEngine::evaluate` gains one `if constexpr` branch per new type.
No existing branches are modified.
