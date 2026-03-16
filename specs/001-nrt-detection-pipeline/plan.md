# Implementation Plan: Near Real-Time Detection Pipeline Framework

**Branch**: `001-nrt-detection-pipeline` | **Date**: 2026-03-15 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/001-nrt-detection-pipeline/spec.md`

## Summary

Build a C++23 embeddable library that provides a configurable, pluggable, multi-tenant
near-real-time detection pipeline. The pipeline composes up to five ordered stages — event
ingest, lightweight evaluation, ML inference, policy evaluation, and decision emission — each
exposing a compile-time concept contract for pluggable evaluators. An optional thin service
harness wraps the library for standalone deployment. Windowed aggregation state defaults to
in-process and can be externalized via a pluggable state-store backend.

## Technical Context

**Language/Version**: C++23 (GCC 14 / Clang 18 minimum; MSVC 19.40 for Windows optional)
**Style**: Coroutines (`co_await` / `co_return`) for async stage execution; C++23 Concepts for
all evaluator and state-store contracts; `std::expected<T, E>` for all fallible operations;
no exceptions anywhere in the framework core; no RTTI required.
**Primary Dependencies**:
- CMake 3.28+ with presets (build system)
- ONNX Runtime 1.19.x C++ API (ML inference stage, dynamic link; static rejected — see research.md)
- standalone Asio 1.30+ (coroutine executor / scheduler; strands as shuffle-shard cells)
- Catch2 v3 (test framework)
- quill 4.x (structured logging; lock-free SPSC hot-path + NDJSON audit sink)
- FlatBuffers (service harness binary serialization; zero-copy decision records)
- nlohmann/json (REST API and debug output only)
- CPM.cmake (dependency management)

**Storage**:
- In-process: time-wheel or ring-buffer windowed aggregation state (default)
- External: pluggable `StateStore` concept backend (Redis adapter et al., optional)

**Testing**: Catch2 v3 — TDD with GIVEN/WHEN/THEN macros; concept-satisfaction tests;
integration tests against full pipeline with synthetic evaluators.

**Target Platform**: Linux x86_64 (primary); macOS arm64/x86_64 (development); Windows x64
(optional, best-effort).

**Project Type**: C++ library (embeddable) + optional standalone service harness binary.

**Performance Goals**:
- P99 end-to-end ≤ 300ms from event receipt to decision emission under production load.
- Per-stage overhead budget: ≤ 10ms for lightweight evaluation; ≤ 200ms for ML inference;
  ≤ 20ms for policy evaluation; ≤ 10ms for emission enqueue. Remaining budget for ingest
  and scheduling overhead.
- Throughput target: sustain ≥ 50,000 events/sec per pipeline instance on a single 8-core
  host (benchmark baseline; tunable by deployment).

**Constraints**:
- Zero exceptions in framework core; all errors propagate via `std::expected<T, Error>`.
- No RTTI; dynamic dispatch only via explicit vtable wrappers at ABI boundaries.
- No synchronous calls to external services on the evaluation hot path (constitution VI).
- Windowed state access MUST be non-blocking on the hot path (lock-free or per-shard mutex).
- Plugin evaluators that violate their timeout MUST be preempted or isolated — cannot block
  the framework's coroutine executor.

**Scale/Scope**: Multi-tenant; designed for 100s of concurrent tenants per instance;
shuffle-sharded worker cells limit single-tenant blast radius.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Specification-First | ✅ PASS | spec.md complete and approved |
| II. Test-First | ✅ PASS | Catch2 TDD enforced; tests written before implementation per tasks.md |
| III. Minimal Dependencies | ✅ PASS | 6 deps justified below; no transitive bloat by design |
| IV. Backward Compatibility & SemVer | ✅ PASS | Public concept contracts are the API surface; versioned accordingly |
| V. Simplicity | ✅ PASS | C++ concepts enforce contracts at compile time — no runtime registry abstraction needed |
| VI. Resiliency & Performance | ✅ PASS | Latency budget defined; failure modes specified per-stage; shuffle sharding and noisy-tenant strategy defined below |

**Dependency justifications (Principle III)**:

| Dependency | Justification |
|------------|--------------|
| ONNX Runtime | Industry-standard model format; no reasonable in-house alternative for FR-006 |
| Asio / stdexec | Coroutine executor required for async stage scheduling; rolling a scheduler violates Principle V |
| Catch2 v3 | Test-first mandate (Principle II) requires a capable C++23 test harness |
| quill 4.x | Zero-overhead structured logging required for FR-012 audit trail; spdlog rejected — hot path formats inline on calling thread (see research.md §9) |
| CPM.cmake | Reproducible dependency fetching; alternative to vcpkg/Conan with lower operational cost |
| Serialization lib | Required only for optional service harness network boundary; not in core library |

**Resiliency gates (Principle VI)**:

- **Latency budget**: P99 ≤ 300ms total; per-stage budgets allocated in Technical Context above.
  Load tests MUST verify budget before merge.
- **Synchronous I/O boundary**: ML inference and policy evaluation invoke pluggable evaluators;
  evaluators MUST NOT make synchronous external service calls — enforced by timeout preemption
  and documented in evaluator contract.
- **Shuffle sharding**: Tenant-to-cell assignment via consistent hashing over K-of-N worker
  cells. Default: N=16 cells, K=4 per tenant. One tenant's runaway load affects at most 4 cells;
  remaining 12 cells serve other tenants unaffected. Strategy documented in data-model.md.
- **Noisy-tenant isolation**: Per-tenant token bucket rate limiter + per-cell concurrency cap.
  Events exceeding the cap are back-pressured (rejected with audit record) rather than queued
  unboundedly.
- **Failure mode analysis**: Per-stage failure modes documented in data-model.md. Each stage
  has a configured `FailureMode` (FailOpen | FailClosed | EmitDegraded). Degraded decisions
  carry a `DegradedReason` bitmask.

## Project Structure

### Documentation (this feature)

```text
specs/001-nrt-detection-pipeline/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── checklists/
│   └── requirements.md
├── contracts/           # Phase 1 output
│   ├── evaluator-contract.md
│   ├── pipeline-config-contract.md
│   ├── decision-record-contract.md
│   └── state-store-contract.md
└── tasks.md             # Phase 2 output (/speckit.tasks — NOT created here)
```

### Source Code (repository root)

```text
include/
└── fre/
    ├── core/
    │   ├── event.hpp            # Event type and metadata
    │   ├── verdict.hpp          # Verdict enum + StageOutput
    │   ├── decision.hpp         # Decision record + DegradedReason
    │   ├── error.hpp            # Error type hierarchy (std::expected)
    │   └── concepts.hpp         # All public evaluator/store concepts
    ├── pipeline/
    │   ├── pipeline.hpp         # Pipeline<Config> assembly + lifecycle
    │   └── pipeline_config.hpp  # Typed configuration DSL
    ├── stage/
    │   ├── ingest_stage.hpp
    │   ├── eval_stage.hpp       # Lightweight evaluation stage
    │   ├── inference_stage.hpp  # ML inference stage
    │   ├── policy_stage.hpp     # Policy evaluation stage
    │   └── emit_stage.hpp       # Decision emission stage
    ├── evaluator/
    │   ├── allow_deny_evaluator.hpp   # Built-in allow/deny list
    │   └── threshold_evaluator.hpp   # Built-in windowed threshold
    ├── state/
    │   ├── window_store.hpp     # In-process time-wheel state store
    │   └── external_store.hpp   # Pluggable external store adapter base
    ├── policy/
    │   └── rule_engine.hpp      # Declarative rule evaluator
    └── sharding/
        └── tenant_router.hpp    # Shuffle sharding + token bucket

src/
├── pipeline/
├── stage/
├── evaluator/
├── state/
├── policy/
└── sharding/

service/                         # Optional standalone service harness
├── include/fre/service/
│   └── harness.hpp
└── src/
    └── main.cpp

tests/
├── contract/                    # Concept satisfaction tests + contract conformance
├── integration/                 # Full pipeline end-to-end tests
└── unit/                        # Per-component unit tests

examples/
├── minimal_pipeline/
└── ml_pipeline/

cmake/
├── FreConfig.cmake.in
└── dependencies.cmake

CMakeLists.txt
CMakePresets.json
```

**Structure Decision**: Single-project library layout. `include/fre/` is the public header tree
(installed with the library). `src/` holds implementation units. `service/` is a sibling
compilation target that depends on the core library. Tests use the installed include path to
mirror consumer experience.

## Complexity Tracking

> Fill ONLY if Constitution Check has violations that must be justified

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|--------------------------------------|
| Shuffle sharding (K-of-N cells) | Multi-tenant blast-radius containment (Constitution VI) | Single shared executor would allow one runaway tenant to degrade all others |
| Pluggable external state store | FR-015: multi-instance shared windowed state | In-process only would prevent horizontal scaling of pipeline instances |
