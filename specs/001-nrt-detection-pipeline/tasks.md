---
description: "Task list for Near Real-Time Detection Pipeline Framework"
---

# Tasks: Near Real-Time Detection Pipeline Framework

**Input**: Design documents from `/specs/001-nrt-detection-pipeline/`
**Prerequisites**: plan.md ✅ spec.md ✅ research.md ✅ data-model.md ✅ contracts/ ✅ quickstart.md ✅

**Tests**: Included — constitution Principle II (Test-First) is non-negotiable; tests are written
before implementation in every user story phase.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks in phase)
- **[Story]**: Which user story this task belongs to (US1–US5)
- Include exact file paths in all task descriptions

## Path Conventions

Single-project library layout per plan.md:

- Public headers: `include/fre/<module>/`
- Implementation: `src/<module>/`
- Tests: `tests/contract/`, `tests/integration/`, `tests/unit/`
- Service harness: `service/`
- Examples: `examples/`
- Build: `cmake/`, `CMakeLists.txt`, `CMakePresets.json`

---

## Phase 1: Setup

**Purpose**: Repository skeleton, build system, and toolchain configuration. No application logic.

- [X] T001 Create full directory tree: `include/fre/{core,pipeline,stage,evaluator,state,policy,sharding,testing}/`, `src/{core,pipeline,stage,evaluator,state,policy,sharding}/`, `tests/{contract,integration,unit}/`, `service/{include/fre/service,src}/`, `examples/{minimal_pipeline,ml_pipeline}/`, `cmake/`, `schemas/`
- [X] T002 [P] Create root `CMakeLists.txt`: open with `project(fre VERSION 1.0.0 LANGUAGES CXX)`; declare `fre` INTERFACE/STATIC library target with `include/` as public include dir; add `service/`, `tests/`, `examples/` subdirectories; set `CMAKE_CXX_STANDARD 23`; expose `fre_VERSION` via `configure_file` into `include/fre/core/version.hpp`
- [X] T003 [P] Create `CMakePresets.json` with four presets: `debug` (no opt, sanitizers), `release` (O3, LTO), `test` (debug + Catch2 test runner), `ci` (release + warnings-as-errors)
- [X] T004 [P] Create `cmake/dependencies.cmake`: bootstrap CPM.cmake via `file(DOWNLOAD ...)`; declare CPM packages for standalone-asio 1.30, Catch2 v3, quill 4.x, FlatBuffers, nlohmann/json; conditionally add ONNX Runtime 1.19 if `FRE_ENABLE_ONNX=ON`
- [X] T005 [P] Create `cmake/FreConfig.cmake.in` and wire `install(TARGETS fre EXPORT FreTargets)` + `install(EXPORT FreTargets NAMESPACE fre:: ...)` in `CMakeLists.txt`
- [X] T006 [P] Create `.clang-format` (C++23 style: `ColumnLimit: 100`, `PointerAlignment: Left`, `SortIncludes: true`) and `.clang-tidy` (enable `modernize-*`, `cppcoreguidelines-*`, disable `*-avoid-c-arrays` for spans)
- [X] T007 [P] Create `tests/CMakeLists.txt`: discover all `tests/**/*.cpp` with `catch_discover_tests()`; link against `fre`, `Catch2::Catch2WithMain`
- [X] T075 [P] Create `CHANGELOG.md` at repo root: document v1.0.0 as initial public release; list the 5-stage pipeline, pluggable evaluator ABI, shuffle-sharded multi-tenant isolation, and in-process/external windowed state as the public API surface; note that all public interfaces are C++23 Concepts (breaking changes require MAJOR version bump per constitution Principle IV)

---

## Phase 2: Foundational

**Purpose**: Core type definitions and infrastructure that ALL user stories depend on. No user story
work begins until this phase is complete.

**⚠️ CRITICAL**: Phases 3–7 all depend on this phase completing first.

- [X] T008 [P] Define `fre::Error` variant type (using `std::variant`) in `include/fre/core/error.hpp`: variants `ConfigError`, `EvaluatorError`, `StoreError`, `EmissionError`, `RateLimitError`, `PipelineError`; each variant carries a `std::string message()` method; no exceptions
- [X] T009 [P] Define `fre::Event` struct in `include/fre/core/event.hpp`: fields `id` (uint64_t), `tenant_id` (std::string — owned, safe for async pipelines), `entity_id` (std::string — owned), `event_type` (std::string — owned), `timestamp` (system_clock::time_point), `payload` (span<const byte> — caller-lifetime), `tags` (span<const Tag> — caller-lifetime); `Tag` is a `{string_view key, string_view value}` pair; NOTE: tenant_id/entity_id/event_type changed from string_view to std::string to prevent dangling-pointer SIGSEGV when async pipeline processing outlives the caller's event variable
- [X] T010 [P] Define `fre::Verdict` enum (`Pass`, `Flag`, `Block`), `fre::EvaluatorResult` struct, `fre::DegradedReason` bitmask enum, and `fre::StageOutput` struct in `include/fre/core/verdict.hpp` per data-model.md
- [X] T011 [P] Define `fre::Decision` struct in `include/fre/core/decision.hpp`: all fields from data-model.md Decision section; `final_verdict` derived as `max(stage verdicts)` helper; `elapsed_us` computed from timestamps
- [X] T012 Define all C++23 Concepts in `include/fre/core/concepts.hpp` (depends on T008–T011): `LightweightEvaluator<E>`, `InferenceEvaluator<E>`, `PolicyEvaluator<E>`, `EmissionTarget<E>`, `StateStore<S>`; each concept checks exact required operation signatures per `contracts/evaluator-contract.md` and `contracts/state-store-contract.md`
- [X] T013 [P] Implement quill logging bootstrap in `include/fre/core/logging.hpp` + `src/core/logging.cpp`: `init_logging(LogConfig)` creates two quill loggers — `diagnostic` (console + rotating file) and `audit` (NDJSON append-only file, one JSON object per line); expose `fre_log_diagnostic(level, fmt, ...)` and `fre_log_audit(Decision&)` macros
- [X] T014 Implement `fre::TenantRouter` in `include/fre/sharding/tenant_router.hpp` + `src/sharding/tenant_router.cpp` (depends on T008): shuffle-shard cell assignment (K-of-N deterministic combinatorial hash, default K=4 N=16); `std::array<asio::strand<asio::thread_pool::executor_type>, N>` cell array; `cells_for(tenant_id)` returns span of K strand references; work-stealing disabled on the underlying `asio::thread_pool`
- [X] T015 Implement lock-free token bucket + concurrency cap in `src/sharding/tenant_router.cpp` (depends on T014): per-tenant `std::atomic<int64_t>` fixed-point token count + `std::atomic<uint64_t>` last-refill timestamp; CAS-loop `try_acquire(tenant_id)` returning `expected<void, RateLimitError>`; `std::atomic<int>` RAII concurrency semaphore per tenant with `max_concurrent` cap
- [X] T016 Write unit tests for core types in `tests/unit/test_core_types.cpp` (depends on T008–T011): verify `Event` field accessors; `Verdict` ordering (`Block > Flag > Pass`); `DegradedReason` bitmask OR; `Decision::elapsed_us` computation
- [X] T017 Write unit tests for `TenantRouter` in `tests/unit/test_tenant_router.cpp` (depends on T014–T015): verify K-of-N cell assignment is deterministic and stable; token bucket allows burst up to capacity then rejects; concurrency cap rejects when in-flight exceeds max; two tenants with non-overlapping cell sets confirmed

**Checkpoint**: Foundation ready — all type definitions, concept contracts, executor infrastructure, and logging are in place. User story phases can now begin in parallel.

---

## Phase 3: User Story 1 — Define and Run a Detection Pipeline (Priority: P1) 🎯 MVP

**Goal**: A platform engineer assembles a minimal pipeline (ingest → eval → emit) via the builder
DSL, submits events, and receives Decision records. No ML inference or policy evaluation required.

**Independent Test**: `tests/integration/test_minimal_pipeline.cpp` — configure a 3-stage pipeline
with a stub allow-all evaluator; submit 10 events; assert 10 decisions emitted, all with
`final_verdict = Pass`, `elapsed_us < 300_000`, and a complete audit trail.

### Tests for User Story 1 ⚠️ Write first — must FAIL before implementation

- [X] T018 [P] [US1] Write contract test in `tests/contract/test_pipeline_contract.cpp`: given a valid 3-stage config, `Pipeline::start()` succeeds; `submit(event)` returns without error; at least one `Decision` is emitted to the registered `EmissionTarget`
- [X] T019 [P] [US1] Write integration test in `tests/integration/test_minimal_pipeline.cpp`: 3-stage pipeline (ingest, eval with stub Pass evaluator, stdout emit); submit 10 events; assert 10 decisions; assert `elapsed_us < 300_000` for all; assert `stage_outputs` has 3 entries per decision

### Implementation for User Story 1

- [X] T020 [P] [US1] Implement `IngestStage` in `include/fre/stage/ingest_stage.hpp` + `src/stage/ingest_stage.cpp`: validate `tenant_id` and `entity_id` non-empty; enforce `skew_tolerance` (reject or flag late events); emit `StageOutput{stage_id=ingest, verdict=Pass}` for valid events; `EmitDegraded` on validation failure
- [X] T021 [P] [US1] Implement `EmitStage` in `include/fre/stage/emit_stage.hpp` + `src/stage/emit_stage.cpp`: accept registered `EmissionTarget` implementations; call `target.emit(Decision&&)` asynchronously off evaluation path; exponential back-off retry up to `retry_limit`; increment `dropped_decisions` counter after max retries; implement `StdoutEmissionTarget` as built-in test target
- [X] T022 [P] [US1] Implement `EvalStage` in `include/fre/stage/eval_stage.hpp` + `src/stage/eval_stage.cpp`: hold `std::vector` of type-erased evaluator handles; invoke each evaluator in sequence; compose verdicts via configured `CompositionRule` (AnyBlock, AnyFlag, Unanimous, Majority); collect into `StageOutput`; enforce per-stage timeout via `asio::cancel_after`
- [X] T023 [US1] Implement `PipelineConfig` builder DSL in `include/fre/pipeline/pipeline_config.hpp` + `src/pipeline/pipeline_config.cpp` (depends on T012, T014): fluent `Builder` class; `build()` validates all rules from `contracts/pipeline-config-contract.md` and returns `expected<PipelineConfig, ConfigError>`; validates stage timeout sum ≤ latency budget; validates emit stage present
- [X] T024 [US1] Implement `Pipeline<Config>` lifecycle in `include/fre/pipeline/pipeline.hpp` + `src/pipeline/pipeline.cpp` (depends on T014, T015, T020–T023): atomic state machine (Stopped → Starting → Running → Draining → Stopped); `start()` validates config and spawns coroutine executor; `submit(Event)` dispatches to tenant's assigned strand via `TenantRouter`; `drain(deadline)` stops accepting new events and flushes in-flight
- [X] T025 [US1] Implement `Pipeline::submit()` event flow in `src/pipeline/pipeline.cpp` (depends on T024): `co_spawn` per event on tenant strand; sequential coroutine chain `co_await ingest → co_await eval → co_await emit`; assemble `Decision` from `StageOutput` vector; compute `elapsed_us`; set `final_verdict` as max of stage verdicts; call `fre_log_audit(decision)` before emission
- [X] T026 [US1] Write unit test for `PipelineConfig` builder in `tests/unit/test_pipeline_config.cpp` (depends on T023): assert `build()` succeeds for valid config; assert `LatencyBudgetExceeded` when stage timeouts sum > budget; assert `RequiredStageMissing` when emit absent; assert `InvalidShardingConfig` when K ≥ N

**Checkpoint**: User Story 1 fully functional and independently testable. A platform engineer can
assemble and run a 3-stage pipeline with no ML or policy stages.

---

## Phase 4: User Story 2 — Plug In a Custom Stage Evaluator (Priority: P1)

**Goal**: A platform engineer implements a custom evaluator satisfying the published concept
contract, registers it, and the pipeline invokes it with the same behavior as a built-in evaluator.

**Independent Test**: `tests/integration/test_custom_evaluator.cpp` — register a synthetic
`BlockAllEvaluator` satisfying `LightweightEvaluator<>`; submit events; assert all decisions carry
`final_verdict = Block` and the evaluator's `reason_code` appears in `stage_outputs`.

### Tests for User Story 2 ⚠️ Write first — must FAIL before implementation

- [X] T027 [P] [US2] Write concept satisfaction tests in `tests/contract/test_evaluator_concepts.cpp`: `static_assert(fre::LightweightEvaluator<SyntheticPassEval>)`, `static_assert(fre::InferenceEvaluator<SyntheticInferenceEval>)`, `static_assert(fre::PolicyEvaluator<SyntheticPolicyEval>)`, `static_assert(fre::EmissionTarget<SyntheticEmitTarget>)`; define minimal synthetic types satisfying each concept
- [X] T028 [P] [US2] Write contract test in `tests/contract/test_custom_evaluator.cpp`: register `SyntheticFlagEvaluator` and `SyntheticBlockEvaluator` on same `EvalStage` with `AnyBlock` composition; submit one event; assert `StageOutput::verdict = Block`; assert both `EvaluatorResult` entries present in `stage_outputs`
- [X] T029 [P] [US2] Write integration test in `tests/integration/test_custom_evaluator.cpp`: `BlockAllEvaluator` registered; 5 events submitted; assert all decisions `final_verdict = Block`; `ErroringEvaluator` registered with `FailOpen` mode; assert degraded decision emitted without crashing concurrent evaluations

### Implementation for User Story 2

- [X] T030 [P] [US2] Implement `CppEvaluatorAdapter<Impl>` in `include/fre/core/concepts.hpp` (extends T012): template wrapper that holds an `Impl` by value; fills `fre_evaluator_vtable_t` at construction time; provides type-erased handle passable to stage registration; `static_assert(LightweightEvaluator<Impl>)` at instantiation
- [X] T031 [P] [US2] Define C vtable ABI in `include/fre/core/plugin_abi.h` (C header, no C++ types): `fre_evaluator_vtable_t` struct with `abi_version`, `evaluator_id`, `void* ctx`, `int (*evaluate)(...)`, `void (*destroy)(ctx)`; define `FRE_EVALUATOR_ABI_VERSION 1`; `extern "C"` factory signature `fre_evaluator_vtable_t* fre_create_evaluator(const char* config_json)`
- [X] T032 [US2] Implement `PluginLoader` in `include/fre/evaluator/plugin_loader.hpp` + `src/evaluator/plugin_loader.cpp` (depends on T031): `dlopen`/`LoadLibrary` wrapper; resolve `fre_create_evaluator` symbol; verify `abi_version == FRE_EVALUATOR_ABI_VERSION`; return `expected<EvaluatorHandle, ConfigError>` on load failure; call `destroy` on unload
- [X] T033 [US2] Extend `EvalStage::add_evaluator()` in `src/stage/eval_stage.cpp` (extends T022, depends on T030): accept both `CppEvaluatorAdapter<Impl>` (same-binary) and `EvaluatorHandle` (plugin-loaded); store as type-erased `std::function<expected<EvaluatorResult, EvaluatorError>(const Event&)>` wrappers; dispatch uniformly
- [X] T034 [US2] Implement per-evaluator failure mode handling in `src/stage/eval_stage.cpp` (depends on T033): on `EvaluatorError::Timeout` or `InternalError`, apply configured `FailureMode`; `FailOpen` → `EvaluatorResult{verdict=Pass, skipped=true}`; `FailClosed` → `EvaluatorResult{verdict=Block, skipped=true}`; `EmitDegraded` → set `DegradedReason` bitmask; never propagate exception or abort other concurrent evaluations
- [X] T035 [US2] Write unit test for multi-evaluator composition in `tests/unit/test_eval_stage_composition.cpp` (depends on T033): two evaluators returning `Flag` and `Block` under each of the four `CompositionRule` variants; assert correct composed `StageOutput::verdict` for each combination

**Checkpoint**: Any type satisfying the evaluator concept integrates without modifying framework
code. Plugin ABI verified. Failure modes tested.

---

## Phase 5: User Story 3 — Configure Windowed Aggregation Thresholds (Priority: P2)

**Goal**: An operator configures a `ThresholdEvaluator` with window duration, aggregation function,
and threshold value. Entities crossing the threshold are flagged; other entities are unaffected.

**Independent Test**: `tests/integration/test_windowed_isolation.cpp` — two tenants each sending
events; one exceeds count threshold at event 101; assert only that tenant's events flagged; other
tenant's events remain `Pass`; assert window resets after expiry.

### Tests for User Story 3 ⚠️ Write first — must FAIL before implementation

- [X] T036 [P] [US3] Write contract test in `tests/contract/test_threshold_contract.cpp`: `ThresholdEvaluator` with 60s window, Count=100; submit 100 events for `entity_A`; assert 100th returns `Pass`; submit 101st; assert returns `Flag` with `reason_code = "threshold_exceeded"`
- [X] T037 [P] [US3] Write integration test in `tests/integration/test_windowed_isolation.cpp`: two entities A and B; A submits 150 events (crossing threshold at 101); B submits 50 events; assert A events 101–150 flagged; assert all B events pass; fast-forward clock past window duration; assert A's next event passes (window reset)

### Implementation for User Story 3

- [X] T038 [P] [US3] Define `WindowKey`, `WindowValue`, `WindowType` (Tumbling/Sliding), `AggregationFn` (Count/Sum/DistinctCount) in `include/fre/state/window_store.hpp` per data-model.md
- [X] T039 [P] [US3] Implement `InProcessWindowStore` in `src/state/window_store.cpp` (depends on T038): power-of-two ring buffer indexed by `epoch = floor(timestamp_ms / window_ms)`; hierarchical time-wheel (256 slots) for expiry; per-shard `std::mutex` (one per TenantRouter cell); `get()`, `compare_and_swap()`, `expire()`, `is_available()` matching `StateStore` concept from `contracts/state-store-contract.md`
- [X] T040 [US3] Implement `ThresholdEvaluator` in `include/fre/evaluator/threshold_evaluator.hpp` + `src/evaluator/threshold_evaluator.cpp` (depends on T038, T039): configurable `window_duration`, `aggregation` (Count/Sum/DistinctCount), `group_by` (EntityId/TenantId/Tag), `threshold`; `evaluate(Event&)` calls `WindowStore::compare_and_swap` to increment aggregate; compares against threshold; returns `Flag` on breach with `metadata{window_count, threshold, window_duration_ms}`; inject `WindowAccessor` via constructor
- [X] T041 [US3] Implement window expiry callback `on_window_expire(WindowKey, WindowValue)` in `src/state/window_store.cpp` (depends on T039): time-wheel tick advance called by background `asio::steady_timer`; expire all windows in current tick slot; notify registered evaluators via callback
- [X] T042 [P] [US3] Implement `ExternalWindowStore` adapter skeleton in `include/fre/state/external_store.hpp` + `src/state/external_store.cpp` (depends on T038): satisfies `StateStore` concept; holds a pluggable backend function table; on `!is_available()` falls back to injected `InProcessWindowStore` and sets `DegradedReason::StateStoreUnavailable`
- [X] T043 [US3] Write unit test for `InProcessWindowStore` in `tests/unit/test_window_store.cpp` (depends on T039): epoch indexing for tumbling window; CAS atomicity under concurrent increments; expiry via time-wheel slot advance; late-arrival event accepted and counted; sliding window slot aggregation

**Checkpoint**: Windowed thresholds fully operational with per-entity, per-shard isolation.
External store interface ready for Redis adapter in polish phase.

---

## Phase 6: User Story 4 — Attach an ML Inference Evaluator (Priority: P2)

**Goal**: A data scientist registers an ONNX model as an `InferenceEvaluator`. The framework invokes
it per event, receives an anomaly score, and produces a flag verdict above the configured threshold.
Timeout is enforced without exceptions; degraded decisions are emitted on timeout.

**Independent Test**: `tests/integration/test_ml_inference_stage.cpp` — register a stub evaluator
returning score 0.9; configure `score_threshold = 0.75`; submit 5 events; assert all flagged with
correct score in `EvaluatorResult`.

### Tests for User Story 4 ⚠️ Write first — must FAIL before implementation

- [X] T044 [P] [US4] Write concept behaviour test in `tests/contract/test_inference_concept.cpp`: verify `InferenceEvaluator` optional `evaluate_batch()` operation — define `SyntheticBatchEval` implementing `evaluate_batch(span<const Event*>)`; confirm `InferenceStage` dispatches to batch path when present; confirm fallback to per-event `evaluate()` when absent (note: `static_assert` coverage for `InferenceEvaluator` already in T027)
- [X] T045 [P] [US4] Write integration test in `tests/integration/test_ml_inference_stage.cpp`: stub returning score 0.9 with threshold 0.75 → all events flagged; stub returning 0.3 → all pass; assert `EvaluatorResult::score` populated in decision `stage_outputs`
- [X] T046 [P] [US4] Write integration test in `tests/integration/test_inference_timeout.cpp`: stub evaluator that sleeps beyond timeout budget; assert decision emitted with `degraded=true`, `DegradedReason::EvaluatorTimeout`, `verdict=Pass` (FailOpen); assert total `elapsed_us < 300_000`

### Implementation for User Story 4

- [X] T047 [P] [US4] Implement `InferenceStage` in `include/fre/stage/inference_stage.hpp` + `src/stage/inference_stage.cpp` (depends on T012, T022): same evaluator registration model as `EvalStage`; `WeightedScore` composition rule: aggregate scores from all evaluators, compare to `score_threshold`, emit `Flag` if above; enforce per-stage timeout via `asio::cancel_after`; on timeout, call `RunOptions::SetTerminate()` via watchdog `std::jthread` and return `EvaluatorError::Timeout`; apply stage `FailureMode`
- [X] T048 [US4] Implement `OnnxInferenceEvaluator` in `include/fre/evaluator/onnx_inference_evaluator.hpp` + `src/evaluator/onnx_inference_evaluator.cpp` (depends on T047): process-global `Ort::Env` singleton (thread-safe after init); one `Ort::Session` per model path (thread-safe for concurrent `Run()`); per-invocation `Ort::RunOptions` + `Ort::IoBinding`; configurable `input_extractor` function to derive `Ort::Value` from `Event`; return `EvaluatorResult{score=output[0]}` on success; wrap `OrtStatus*` error into `EvaluatorError::InternalError`
- [X] T049 [US4] Implement optional `evaluate_batch()` in `OnnxInferenceEvaluator` (depends on T048): accept `span<const Event*>`; batch `Ort::Value` inputs; single `Session::Run()`; return `vector<EvaluatorResult>`; `InferenceStage` batches events per evaluator when `evaluate_batch` present
- [X] T050 [US4] Wire `InferenceStage` into `Pipeline` between `EvalStage` and `PolicyStage` in `src/pipeline/pipeline.cpp` (depends on T047, T025): extend coroutine chain `co_await ingest → co_await eval → co_await inference → co_await emit`; inference stage is optional (skipped if not configured)

**Checkpoint**: ML inference integrated, timeout-safe, with degraded decision on stall. ONNX
model evaluators register without framework changes.

---

## Phase 7: User Story 5 — Evaluate Policy Rules Against Stage Outputs (Priority: P3)

**Goal**: A security engineer authors declarative policy rules that combine stage verdicts and
evaluator scores using AND/OR/NOT logic. The policy evaluation stage applies rules in priority
order and emits a block verdict when rules match.

**Independent Test**: `tests/integration/test_policy_evaluation.cpp` — 4-stage pipeline
(ingest, eval with Flag evaluator, inference with score 0.9, policy); rule: `AND(eval=Flag, score>0.8)`
→ Block; submit event; assert `final_verdict = Block` with `matched_rule` in policy stage output.

### Tests for User Story 5 ⚠️ Write first — must FAIL before implementation

- [X] T051 [P] [US5] Write `PolicyContext` behaviour test in `tests/contract/test_policy_concept.cpp`: verify `PolicyEvaluator` receives a correctly populated `PolicyContext` — assert `context.stage_outputs` contains entries for all preceding active stages; assert `context.event` reference matches submitted event (note: `static_assert` coverage for `PolicyEvaluator` already in T027)
- [X] T052 [P] [US5] Write contract test in `tests/contract/test_policy_rule_contract.cpp`: `PolicyContext` with EvalStage=Flag and InferenceEvaluator score=0.9; rule `And(StageVerdictIs(eval, Flag), EvaluatorScoreAbove(anomaly, 0.8))`; assert rule matches and returns `Block` verdict
- [X] T053 [P] [US5] Write integration test in `tests/integration/test_policy_evaluation.cpp`: 4-stage pipeline; composite AND rule; submit event satisfying both conditions; assert `final_verdict = Block`, `matched_rule` in policy `EvaluatorResult::metadata`
- [X] T054 [P] [US5] Write integration test in `tests/integration/test_policy_config_validation.cpp`: policy rule referencing `StageId::ml_inference` on pipeline with no `InferenceStage` configured; assert `Pipeline::start()` returns `ConfigError::UndefinedStageDependency`

### Implementation for User Story 5

- [X] T055 [P] [US5] Define `PolicyRule` AST node types in `include/fre/policy/rule_engine.hpp` (depends on T010): `StageVerdictIs{stage_id, verdict}`, `EvaluatorScoreAbove{evaluator_id, threshold}`, `TagEquals{key, value}`, `And{left, right}`, `Or{left, right}`, `Not{expr}`; use `std::variant` for node union; `PolicyContext` holds `span<const StageOutput>` and `const Event&`
- [X] T056 [US5] Implement `RuleEngine::evaluate(PolicyContext, PolicyRule)` in `src/policy/rule_engine.cpp` (depends on T055): recursive `std::visit` over AST variant; `StageVerdictIs` → look up stage in context span; `EvaluatorScoreAbove` → scan `EvaluatorResult` entries for matching `evaluator_id`; `TagEquals` → scan `Event::tags`; return `bool`
- [X] T057 [US5] Implement `PolicyStage` in `include/fre/stage/policy_stage.hpp` + `src/stage/policy_stage.cpp` (depends on T056): hold `std::vector<PolicyRule>` sorted by `priority` ascending; evaluate rules in order; first matching rule determines verdict and `action`; `FailClosed` failure mode (default) on evaluator error; set `DegradedReason::PartialEvaluation` on timeout
- [X] T058 [US5] Extend `PipelineConfig` startup validation in `src/pipeline/pipeline_config.cpp` (extends T023, depends on T057): for each `PolicyRule`, walk AST and collect referenced `StageId` values; assert each referenced stage is present in config; return `ConfigError::UndefinedStageDependency(rule_id, stage_id)` on first violation
- [X] T059 [US5] Wire `PolicyStage` into `Pipeline` in `src/pipeline/pipeline.cpp` (depends on T057, T050): extend coroutine chain to `co_await ingest → co_await eval → co_await inference → co_await policy → co_await emit`; policy stage optional (skipped if not configured); `PolicyContext` constructed from accumulated `StageOutput` vector before policy invocation

**Checkpoint**: All 5 stages wired. Full 5-stage pipeline with composite policy logic operational.

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Capabilities that complete the framework but do not belong to a single user story.

- [X] T060 [P] Write unit test for `AllowDenyEvaluator` in `tests/unit/test_allow_deny_evaluator.cpp` (write first — must FAIL): entity on allow-list → `Pass`; entity on deny-list → `Block`; entity on neither → configurable default (`Pass` or `Flag`); missing list file at construction → `ConfigError::EvaluatorLoadFailed`
- [X] T071 [P] Implement `AllowDenyEvaluator` in `include/fre/evaluator/allow_deny_evaluator.hpp` + `src/evaluator/allow_deny_evaluator.cpp` (depends on T060): load allow/deny list from file path at construction; configurable `match` field (`entity_id`, `tenant_id`, or tag key); `evaluate(Event&)` O(1) `unordered_set` lookup; return `Pass` for allow-list match, `Block` for deny-list match, configurable default for neither
- [X] T072 [P] Write integration test for `ExternalWindowStore` fallback in `tests/integration/test_external_store_fallback.cpp` (write first — must FAIL): configure pipeline with `ExternalWindowStore` whose `is_available()` returns `false`; submit events; assert decisions emitted with `DegradedReason::StateStoreUnavailable`; assert in-process state used as fallback (window counts still increment correctly)
- [X] T061 [P] Implement Redis `ExternalWindowStore` adapter in `src/state/redis_window_store.cpp` (depends on T072): implement `StateStore` concept using `hiredis` or `redis-plus-plus`; `GET` key → `get()`; `SET key value PX ttl XX` with `version` field (optimistic lock) → `compare_and_swap()`; `DEL key` → `expire()`; `PING` (result cached 100ms) → `is_available()`; MessagePack for `WindowValue` serialization
- [X] T062 [P] Define FlatBuffers schema in `schemas/decision.fbs`: all `Decision` fields per `contracts/decision-record-contract.md`; run `flatc --cpp` in CMake `add_custom_command`; generate `include/fre/service/decision_generated.h`
- [X] T063 [P] Implement `DecisionSerializer` in `src/service/decision_serializer.cpp` (depends on T062): `to_flatbuffer(Decision&) → vector<uint8_t>`; `to_json(Decision&) → string` (nlohmann/json); `from_flatbuffer(span<const uint8_t>) → expected<Decision, Error>`
- [X] T064 [P] Implement `PipelineTestHarness` in `include/fre/testing/pipeline_harness.hpp`: wraps `Pipeline`; captures emitted `Decision` records in a `std::vector`; `submit_events(span<Event>)`; `wait_for_decisions(count, timeout) → expected<span<const Decision>, Error>`; suitable for integration tests and quickstart validation
- [X] T073 [P] Write integration test for service harness endpoints in `tests/integration/test_service_harness.cpp` (write first — must FAIL): start harness with minimal pipeline config; `POST /events` with valid JSON event → assert 202 and decision emitted; `GET /health` → assert JSON body contains `state` and `degraded` fields; `POST /pipeline/drain` → assert pipeline drains and health reports `stopped`
- [X] T065 Implement service harness binary in `service/src/main.cpp` + `service/include/fre/service/harness.hpp` (depends on T062–T063, T073): minimal HTTP server using Asio + hand-rolled HTTP/1.1 (no Beast — avoids Boost dependency per Principle III); `POST /events` → deserialize event, call `Pipeline::submit()`, return 202; `GET /health` → return pipeline state + degradation status as JSON; `POST /pipeline/drain` → call `Pipeline::drain()`; load pipeline config from JSON file at startup
- [X] T066 [P] Create `examples/minimal_pipeline/main.cpp`: demonstrates builder DSL, `ThresholdEvaluator`, `StdoutEmissionTarget`; matches `quickstart.md` Step 2
- [X] T067 [P] Create `examples/ml_pipeline/main.cpp`: demonstrates `OnnxInferenceEvaluator` + `PolicyRule`; uses `PipelineTestHarness`; matches `quickstart.md` Steps 4–5
- [X] T068 [P] Write P99 latency `BENCHMARK` in `tests/unit/test_latency_benchmark.cpp`: 5-stage pipeline with stub evaluators at each stage; `BENCHMARK` submits 10,000 events; assert `BENCHMARK` median < 50ms and p99 < 300ms (via `Catch::BenchmarkStats`)
- [X] T074 Write sustained load test in `tests/integration/test_load_p99.cpp` (constitution VI gate — required before merge): 10 tenants × 5,000 events each (50,000 total); submit all events concurrently via `std::jthread` pool; collect all emitted `Decision::elapsed_us` values; assert p99 of collected values ≤ 300,000µs; assert zero decisions missing (`count == 50,000`); assert no tenant's events blocked by another tenant (per-tenant p99 each ≤ 300,000µs) (depends on T064)
- [X] T069 Add `install()` targets in `CMakeLists.txt` (extends T002): `install(TARGETS fre EXPORT FreTargets)`; `install(DIRECTORY include/ DESTINATION include)`; `install(EXPORT FreTargets NAMESPACE fre:: DESTINATION lib/cmake/fre)`; `configure_package_config_file(cmake/FreConfig.cmake.in ...)` with `find_dependency(asio)` and optional `find_dependency(onnxruntime)`
- [X] T070 Validate `quickstart.md`: build `examples/minimal_pipeline` and `examples/ml_pipeline` under `test` CMake preset; assert both compile and link without errors; run and assert zero-exit

### Quality Gates (FR-016 – FR-019) ⚠️ Added post-clarification

- [X] T076 [P] Add three sanitizer CMake presets to `CMakePresets.json` (extends T003, covers FR-016): `asan` preset inherits `debug`, appends `-fsanitize=address,leak -fno-omit-frame-pointer` to `CMAKE_CXX_FLAGS`; `ubsan` preset inherits `debug`, appends `-fsanitize=undefined -fno-sanitize-recover=all`; `tsan` preset inherits `debug`, appends `-fsanitize=thread`; each preset sets `ASAN_OPTIONS=detect_leaks=1`, `UBSAN_OPTIONS=print_stacktrace=1`, `TSAN_OPTIONS=halt_on_error=1` via `cacheVariables`; note: sanitizer presets are mutually exclusive — do NOT combine ASan+TSan in one build
- [X] T077 [P] Create `scripts/ci/run-sanitizers.sh` (covers FR-016 CI gate): bash script that sequentially builds with `cmake --preset asan`, `cmake --preset ubsan`, `cmake --preset tsan` then runs `ctest --preset asan` / `ctest --preset ubsan` / `ctest --preset tsan`; exits non-zero on any sanitizer failure; each run's output saved to `build/sanitizer-{asan,ubsan,tsan}.log`; script is the blocking CI quality gate for FR-016
- [X] T078 [P] Add `coverage` CMake preset to `CMakePresets.json` and coverage report target to `CMakeLists.txt` (covers FR-017): `coverage` preset inherits `debug`, adds `-fprofile-arcs -ftest-coverage` (GCC) or `--coverage` (Clang) to `CMAKE_CXX_FLAGS` and `CMAKE_EXE_LINKER_FLAGS`; add `coverage` custom CMake target in `CMakeLists.txt` that: runs `ctest --preset coverage`, invokes `lcov --capture --directory . --output-file build/coverage/coverage.info`, excludes `_deps/` and `tests/`, generates HTML via `genhtml`; Clang path uses `llvm-cov gcov` wrapper
- [X] T079 [P] Create `scripts/ci/check-coverage.sh` (covers FR-017 threshold gate): bash script that parses `lcov --summary` output, extracts the "lines" percentage, and exits non-zero if the value is below 80.0%; prints a human-readable report with current coverage and the 80% threshold; called from CI after the `coverage` CMake target completes; this script is the blocking CI quality gate for FR-017
- [X] T080 [P] Wire `CMAKE_CXX_CLANG_TIDY` in `CMakeLists.txt` for the `ci` preset (covers FR-018): add `option(FRE_CLANG_TIDY "Enable clang-tidy during build" OFF)` to root `CMakeLists.txt`; when `ON`, set `CMAKE_CXX_CLANG_TIDY "clang-tidy;--warnings-as-errors=*"` globally; update the `ci` CMakePreset to set `FRE_CLANG_TIDY=ON` via `cacheVariables`; ensure the existing `.clang-tidy` from T006 is in the repo root so clang-tidy discovers it automatically; build under the `ci` preset is the blocking quality gate for FR-018
- [X] T081 [P] Create `scripts/ci/run-valgrind.sh` and `valgrind.supp` suppression file (covers FR-019): `run-valgrind.sh` runs `valgrind --tool=memcheck --error-exitcode=0 --suppressions=valgrind.supp --xml=yes --xml-file=build/valgrind-report.xml <test_binary>` for each test executable discovered under `build/debug/tests/`; script always exits 0 (non-blocking advisory); `valgrind.supp` pre-suppresses known false-positive sources in quill and asio SPSC internals; CI job archives `build/valgrind-report.xml` as a build artifact for review

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately
- **Foundational (Phase 2)**: Depends on Phase 1 completion — BLOCKS all user stories
- **US1 (Phase 3)**: Depends on Phase 2 — no dependency on US2–US5
- **US2 (Phase 4)**: Depends on Phase 3 (extends `EvalStage`) — no dependency on US3–US5
- **US3 (Phase 5)**: Depends on Phase 2 — no dependency on US1/US2 (standalone `ThresholdEvaluator`)
- **US4 (Phase 6)**: Depends on Phase 3 (`Pipeline` wiring) — no dependency on US2/US3/US5
- **US5 (Phase 7)**: Depends on Phases 3, 4, and 6 (uses `EvalStage`, `InferenceStage`, `Pipeline`)
- **Polish (Phase 8)**: Depends on all user story phases complete

### User Story Dependencies

- **US1 (P1)**: Starts after Phase 2 — no story dependencies
- **US2 (P1)**: Starts after US1 — extends `EvalStage` built in US1
- **US3 (P2)**: Starts after Phase 2 — independent of US1/US2 (own `WindowStore` + `ThresholdEvaluator`)
- **US4 (P2)**: Starts after US1 — extends `Pipeline` coroutine chain built in US1
- **US5 (P3)**: Starts after US1, US2, US4 — `PolicyStage` references `EvalStage` and `InferenceStage`

### Within Each User Story

1. Tests written and confirmed FAILING before any implementation task begins
2. Type definitions / header files before implementation units
3. Stage implementation before pipeline wiring
4. Unit tests before integration tests

### Parallel Opportunities

- All Phase 1 `[P]` tasks run simultaneously
- All Phase 2 `[P]` tasks run simultaneously (T008–T011 produce independent headers)
- US1, US3 can start in parallel after Phase 2 (fully independent)
- US1 and US4 stage implementations (`T020`, `T021`, `T022`) run in parallel
- All US2 test tasks `[P]` run in parallel
- All Phase 8 `[P]` tasks run in parallel after Phase 7

---

## Parallel Example: User Story 1

```bash
# Write tests first (all [P] — different files):
Task T018: tests/contract/test_pipeline_contract.cpp
Task T019: tests/integration/test_minimal_pipeline.cpp

# Run tests — confirm they FAIL (no implementation yet)

# Implement stages in parallel (all [P] — different files):
Task T020: src/stage/ingest_stage.cpp
Task T021: src/stage/emit_stage.cpp
Task T022: src/stage/eval_stage.cpp

# Then sequentially (each depends on previous):
Task T023: src/pipeline/pipeline_config.cpp   (depends on T020-T022)
Task T024: src/pipeline/pipeline.hpp/.cpp     (depends on T023)
Task T025: Pipeline::submit() flow            (depends on T024)
Task T026: unit test for PipelineConfig       (depends on T023)

# Run tests — confirm they PASS
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL — blocks everything)
3. Complete Phase 3: US1 (ingest → eval → emit)
4. **STOP AND VALIDATE**: run `tests/integration/test_minimal_pipeline` — 10 events, all decisions emitted, P99 < 300ms
5. A functioning pipeline framework exists at this point

### Incremental Delivery

1. Setup + Foundational → types, executor, logging ready
2. US1 → minimal pipeline running → **MVP**
3. US2 → custom evaluators pluggable → first extension point
4. US3 (parallel with US4) → windowed thresholds → volumetric detection
5. US4 (parallel with US3) → ML inference → anomaly detection
6. US5 → policy rules → composite decisioning
7. Polish → Redis adapter, service harness, examples, benchmarks

### Parallel Team Strategy (4 engineers after Phase 2)

- **Engineer A**: US1 (pipeline shell, stages, lifecycle)
- **Engineer B**: US2 (concepts, C vtable, plugin loader)
- **Engineer C**: US3 (window store, threshold evaluator)
- **Engineer D**: US4 (inference stage, ONNX evaluator)

US5 and Polish integrate their work once all four complete.

---

## Notes

- `[P]` tasks touch different files — no merge conflicts when run in parallel
- `[Story]` label maps each task to its user story for traceability and independent delivery
- Tests are written FIRST and run to confirm failure before any implementation task in that story
- Commit after each task or logical group; commit message format: `type(US?): description`
- Verify P99 latency benchmark (T068) passes before marking Polish complete
- Constitution Principle VI resiliency gate: T068 (latency benchmark), T043 (windowed isolation),
  T034 (failure modes) are the three tasks that directly verify gate compliance
