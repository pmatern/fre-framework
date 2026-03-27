# Contract: PolicyRule Extended Leaf Nodes

**Scope**: `include/fre/policy/rule_engine.hpp` — `PolicyRule::Variant` additions
**Stability**: Public API (MINOR version bump; backward-compatible)

---

## Existing Contract (unchanged)

`RuleEngine::evaluate(ctx, rule) -> bool` is a pure function. It:
- Never throws.
- Returns `true` if the rule matches the context; `false` otherwise.
- Is stateless — safe to call from any thread or strand concurrently.

These guarantees extend to all new node types below.

---

## New Node Contracts

### TagContains

```
Preconditions : key and substring are non-empty strings (caller responsibility)
Postconditions: returns true iff event carries a tag with key==key AND
                tag.value.find(substring) != npos
Edge cases    : absent tag → false; empty substring always matches any present tag
```

### TagStartsWith

```
Preconditions : key and prefix are non-empty strings
Postconditions: returns true iff event carries tag with key==key AND
                tag.value.starts_with(prefix)
Edge cases    : absent tag → false; empty prefix always matches any present tag
```

### TagIn

```
Preconditions : values may be empty
Postconditions: returns true iff event carries tag with key==key AND
                tag.value is an exact member of values
Edge cases    : absent tag → false; empty values set → always false
```

### TagExists

```
Preconditions : key is non-empty
Postconditions: returns true iff event carries any tag with key==key (value ignored)
Edge cases    : absent tag → false
```

### TagValueLessThan

```
Preconditions : key non-empty; threshold is a finite double
Postconditions: parses tag.value as double via std::from_chars;
                returns true iff parse succeeds AND parsed_value < threshold
Edge cases    : absent tag → false; non-numeric value → false; NaN/inf → false
```

### TagValueGreaterThan

```
Preconditions : key non-empty; threshold is a finite double
Postconditions: parses tag.value as double via std::from_chars;
                returns true iff parse succeeds AND parsed_value > threshold
Edge cases    : absent tag → false; non-numeric value → false
```

### TagValueBetween

```
Preconditions : key non-empty; lo and hi are finite doubles
Postconditions: parses tag.value as double via std::from_chars;
                returns true iff parse succeeds AND lo <= parsed_value < hi
Edge cases    : absent tag → false; non-numeric → false; lo >= hi → always false
```

### EventTypeIs

```
Preconditions : event_type is non-empty
Postconditions: returns true iff ctx.event.event_type == event_type
Edge cases    : none
```

### EventTypeIn

```
Preconditions : event_types may be empty
Postconditions: returns true iff ctx.event.event_type is an exact member of event_types
Edge cases    : empty set → always false
```

### TenantIs

```
Preconditions : tenant_id is non-empty
Postconditions: returns true iff ctx.event.tenant_id == tenant_id
Edge cases    : none
```

### EventOlderThan

```
Preconditions : duration >= 0
Postconditions: computes age = system_clock::now() - ctx.event.timestamp;
                returns true iff age > duration
Edge cases    : future timestamp (age < 0) → false; zero duration → false
```

### EventNewerThan

```
Preconditions : duration >= 0
Postconditions: computes age = system_clock::now() - ctx.event.timestamp;
                returns true iff age >= 0 AND age < duration
Edge cases    : future timestamp → false (age negative); zero duration → false
```

### EvaluatorScoreBetween

```
Preconditions : evaluator_id non-empty; lo and hi are finite floats
Postconditions: scans all stage_outputs and their evaluator_results for evaluator_id;
                returns true iff a matching result has score.has_value() AND
                lo <= *score < hi
Edge cases    : evaluator not found → false; score absent → false; lo >= hi → false
```

### StageIsDegraded

```
Preconditions : stage_id non-empty
Postconditions: scans stage_outputs for stage_id;
                returns true iff matching stage has is_degraded(degraded_reason) == true
Edge cases    : stage not found → false
```

### EvaluatorWasSkipped

```
Preconditions : evaluator_id non-empty
Postconditions: scans all stage_outputs for evaluator result with evaluator_id;
                returns true iff matching result has skipped == true
Edge cases    : evaluator not found → false
```

### EvaluatorReasonIs

```
Preconditions : evaluator_id and reason_code are non-empty
Postconditions: scans all stage_outputs for evaluator result with evaluator_id;
                returns true iff result.reason_code.has_value() AND
                *result.reason_code == reason_code
Edge cases    : evaluator not found → false; reason_code absent → false
```

---

## Composability Contract

All new leaf nodes satisfy the existing composability contract:
- Implicitly convertible to `PolicyRule` via the converting constructors.
- Usable as `L` or `R` template arguments to `And{...}` and `Or{...}`.
- Usable as `E` template argument to `Not{...}`.
- Usable directly in `PolicyStageConfig::add_rule(PolicyRule{...}, ...)`.

No changes to `And`, `Or`, `Not`, or `PolicyRule` copy operations are required.
