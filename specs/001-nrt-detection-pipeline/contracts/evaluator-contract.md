# Contract: Pluggable Evaluator

**Version**: 1.0.0
**Stage applicability**: All stages (each stage defines a specialised concept)

## Overview

Every pluggable evaluator satisfies a C++23 concept. The concept is the contract. There is no
base class, no virtual dispatch in the core hot path, and no exceptions. All evaluators are
registered by value (or owning pointer) into a stage configuration; the framework invokes them
via the concept interface.

For ABI-stable dynamic loading (shared library plugins), a C vtable shim wraps the concept
into a stable C struct. Same-binary evaluators satisfy the concept directly — no shim needed.

---

## Concept: `LightweightEvaluator<E>`

Satisfied by any type that can synchronously assess one event within a tight time budget.

### Required operations

```
evaluate(const Event&) -> expected<EvaluatorResult, EvaluatorError>
```

- MUST return within the per-stage timeout (typically ≤ 10ms).
- MUST NOT perform synchronous I/O to external services.
- MUST NOT throw.
- MAY read from in-process state (e.g., allow/deny list loaded at startup).
- MAY read from in-process windowed aggregation state via an injected `WindowAccessor`.

### Optional operations (framework uses if present)

```
on_window_expire(WindowKey, WindowValue) -> void
```
Called by the framework when a window expires; allows the evaluator to reset derived state.

---

## Concept: `InferenceEvaluator<E>`

Satisfied by any type that can evaluate one event (or a batch) against an ML model and return
a score. Invoked inside the ML inference stage.

### Required operations

```
evaluate(const Event&) -> expected<EvaluatorResult, EvaluatorError>
```

- The framework enforces an external timeout around this call. If the call does not return
  within the stage's allocated budget, the framework cancels via cooperative cancellation
  token and applies the stage's `FailureMode`.
- MUST NOT throw.
- MAY call external model serving infrastructure, subject to the timeout constraint.
- Score field in `EvaluatorResult` SHOULD be set to a value in [0.0, 1.0].

### Optional batch operation

```
evaluate_batch(std::span<const Event*>) -> expected<std::vector<EvaluatorResult>, EvaluatorError>
```

When present, the framework MAY batch events per this evaluator to amortise model invocation
overhead. Batch size is configurable in the stage config.

---

## Concept: `PolicyEvaluator<E>`

Satisfied by any type that evaluates a `PolicyRule` expression against the accumulated
`StageOutput` set for one event.

### Required operations

```
evaluate(const PolicyContext&) -> expected<EvaluatorResult, EvaluatorError>
```

`PolicyContext` carries the event and all preceding `StageOutput` values, keyed by `StageId`.

- MUST be deterministic for the same inputs.
- MUST NOT perform I/O.
- MUST NOT throw.

---

## Concept: `EmissionTarget<E>`

Satisfied by any type that can accept a `Decision` record for delivery to a downstream consumer.

### Required operations

```
emit(Decision&&) -> expected<void, EmissionError>
```

- Called asynchronously off the evaluation hot path.
- MUST NOT block the caller beyond a configurable emission timeout.
- On error, the framework retries with exponential back-off up to a configured limit, then
  drops and increments a `dropped_decisions` metric.

### Optional operations

```
flush() -> expected<void, EmissionError>
```

Called during pipeline `drain()` to ensure in-flight decisions are delivered before shutdown.

---

## ABI-Stable Plugin C Interface

For evaluators loaded at runtime from shared libraries, the following C struct provides an
ABI-stable vtable. The framework wraps a concept-satisfying type into this struct automatically
via a provided adapter template.

```c
typedef struct fre_evaluator_vtable {
    uint32_t abi_version;           /* Must equal FRE_EVALUATOR_ABI_VERSION */
    const char* evaluator_id;       /* Null-terminated, stable for lifetime of plugin */
    void* ctx;                      /* Opaque evaluator state */

    /* Returns 0 on success; fills result; fills err_buf on failure */
    int (*evaluate)(
        void* ctx,
        const fre_event_t* event,
        fre_evaluator_result_t* result,
        char* err_buf,
        size_t err_buf_len
    );

    void (*destroy)(void* ctx);     /* Called when evaluator is unregistered */
} fre_evaluator_vtable_t;
```

Plugins export a factory function:

```c
fre_evaluator_vtable_t* fre_create_evaluator(const char* config_json);
```

The framework calls `fre_create_evaluator` at pipeline startup and `destroy` at shutdown.

---

## Error Types

| Error | When emitted | Framework action |
|-------|-------------|-----------------|
| `EvaluatorError::Timeout` | Evaluator did not return within budget | Apply stage FailureMode |
| `EvaluatorError::InvalidInput` | Evaluator rejected the event payload | FailOpen + audit log |
| `EvaluatorError::InternalError` | Evaluator returned unexpected error | Apply stage FailureMode |
| `EvaluatorError::NotReady` | Evaluator is initialising (e.g., model loading) | FailOpen + audit log |

---

## Versioning

The evaluator concept interface is versioned via `FRE_EVALUATOR_ABI_VERSION`. Breaking changes
to the C vtable require a MAJOR version bump of the framework (constitution Principle IV).
Concept-based same-binary evaluators recompile against the new headers and gain compile-time
concept check errors on breaking changes.
