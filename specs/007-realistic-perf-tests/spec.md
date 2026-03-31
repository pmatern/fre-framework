# Feature Specification: Realistic Performance Tests with DuckDB

**Feature Branch**: `007-realistic-perf-tests`
**Created**: 2026-03-29
**Status**: Draft
**Input**: User description: "add two more tests along the lines of tests/unit/test_latency_benchmark.cpp and tests/integration/test_load_p99.cpp but add real evaluator implementations. the onyxevaluator usage can be a placeholder for now, but the others should actually test something about the input events. they should also actually write to and read from duckdb locally. The tests should clean up resources after they finish."

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Latency Benchmark with Real Evaluators and On-Disk DuckDB (Priority: P1)

A developer running the benchmark suite needs a Catch2 `BENCHMARK` test that measures pipeline p50/p99 latency under realistic conditions: a `ThresholdEvaluator` backed by `WriteBackWindowStore` (hot in-memory + warm on-disk DuckDB), an `AllowDenyEvaluator` that checks event identity against a deny list, and a stub placeholder for the ONNX inference stage. Events are meaningfully differentiated — some belong to a known-bad entity on the deny list, others accumulate toward a count threshold, and the rest pass freely. After the benchmark completes, all temporary DuckDB files are deleted.

**Why this priority**: The existing `test_latency_benchmark.cpp` measures only framework overhead with no-op evaluators. This test gives the first real signal on how evaluator work — windowed counting, list lookups, async DuckDB flushes — affects p50/p99 under benchmark conditions.

**Independent Test**: Can be run alone via the `[benchmark][realistic]` tag. Delivers a standalone p99 measurement with real evaluators and a local DuckDB file.

**Acceptance Scenarios**:

1. **Given** a pipeline with `AllowDenyEvaluator` (deny list containing `"blocked-entity"`) and `ThresholdEvaluator<WriteBackWindowStore>` (threshold = 200 counts per window, DuckDB at a temp file path), **When** 1000 events are submitted (mix of blocked, threshold-crossing, and normal entities) and all decisions collected, **Then** the Catch2 benchmark reports a p99 latency value derived from `Decision::elapsed_us`.
2. **Given** events for `"blocked-entity"`, **When** decisions are collected, **Then** those decisions carry `Block` verdicts.
3. **Given** events for an entity whose count has crossed the threshold, **When** decisions are collected, **Then** at least some carry `Flag` verdicts.
4. **Given** the benchmark has completed, **When** the test fixture is torn down, **Then** the temporary DuckDB file no longer exists on disk.

---

### User Story 2 — Sustained Load Test with Real Evaluators and Per-Tenant DuckDB Persistence (Priority: P1)

A developer running the integration test suite needs a sustained-load test analogous to `test_load_p99.cpp` that replaces no-op evaluators with real ones. The pipeline uses `ThresholdEvaluator<WriteBackWindowStore>` (DuckDB on a temp file), `AllowDenyEvaluator` (deny list with one known-bad entity per tenant), and a placeholder inference evaluator. Ten tenants submit events concurrently. The test asserts both overall and per-tenant P99 ≤ 500ms, verifies that DuckDB received writes by confirming at least one `Flag` verdict per tenant, and deletes temp files on completion.

**Why this priority**: The existing `test_load_p99.cpp` proves the plumbing holds under load but says nothing about correctness or performance when evaluators do real work. This test closes that gap and establishes a latency budget for the full evaluator stack.

**Independent Test**: Can be run alone via the `[integration][load][realistic]` tags. Delivers a pass/fail gate confirming SLA adherence and correct verdict classification under concurrent load.

**Acceptance Scenarios**:

1. **Given** 10 tenants each submitting 3000 events concurrently, with one denied entity per tenant and a threshold of 200 events per entity window backed by on-disk DuckDB, **When** all 30 000 decisions are collected, **Then** overall P99 ≤ 500ms and per-tenant P99 ≤ 500ms.
2. **Given** entities whose cumulative event count crosses 200 within the window, **When** decisions are collected, **Then** at least one `Flag` verdict exists per tenant, confirming threshold state was tracked and persisted to DuckDB.
3. **Given** events for denied entities (one per tenant), **When** decisions are collected, **Then** those decisions carry `Block` verdicts.
4. **Given** the test has completed (pass or fail), **When** the test fixture is torn down, **Then** the temporary DuckDB file does not exist on disk.

---

### Edge Cases

- What happens when the DuckDB background flush fires mid-benchmark while events are still in flight — does it cause latency spikes?
- What if the system temp directory is not writable — does the test fail with a clear error rather than hanging?
- Do concurrent CAS retries in `ThresholdEvaluator` under high contention inflate p99 beyond the SLA?
- Does deny-list evaluation take precedence over threshold evaluation so a blocked entity is never also flagged?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Both tests MUST be gated behind `#ifdef FRE_ENABLE_DUCKDB` and only compiled when the `duckdb` CMake preset is active.
- **FR-002**: Both tests MUST create a temporary DuckDB database file and a temporary deny-list file at unique paths under the system temp directory, and MUST delete both files in a cleanup step that executes regardless of test pass/fail.
- **FR-003**: Both tests MUST include an `AllowDenyEvaluator` configured with a deny list containing at least one entity ID, and MUST verify that events matching the deny list produce `Block` verdicts.
- **FR-004**: Both tests MUST include a `ThresholdEvaluator<WriteBackWindowStore>` where the `WriteBackWindowStore` is backed by an `InProcessWindowStore` (hot) and the on-disk `DuckDbWindowStore` (warm). The threshold MUST be set low enough that it is crossed during the test run, producing at least one `Flag` verdict.
- **FR-005**: Both tests MUST include a stub/placeholder evaluator in the inference stage (no-op pattern) until a real ONNX model is available.
- **FR-006**: The benchmark test MUST use the Catch2 `BENCHMARK` macro and return the p99 latency value so it appears in Catch2 benchmark output, consistent with `test_latency_benchmark.cpp`.
- **FR-007**: The load test MUST assert that overall P99 latency ≤ 500ms and per-tenant P99 ≤ 500ms across all 10 tenants.
- **FR-008**: The load test MUST assert that the total number of collected decisions equals the total number of submitted events (30 000), confirming no silent drops under load.
- **FR-009**: Both tests MUST warm up the pipeline by submitting and discarding a small batch of events before measuring or asserting latency.

### Key Entities

- **Benchmark test file**: New file at `tests/unit/test_realistic_latency_benchmark.cpp`, mirroring `test_latency_benchmark.cpp` with real evaluators and on-disk DuckDB.
- **Load test file**: New file at `tests/integration/test_realistic_load_p99.cpp`, mirroring `test_load_p99.cpp` with real evaluators and on-disk DuckDB.
- **Temporary DuckDB file**: Created at test start under the system temp directory; deleted unconditionally at test teardown via a RAII guard or equivalent.
- **Deny list**: A small fixed set of entity IDs written to a temp file at test startup and deleted at teardown (alongside the DuckDB file). `AllowDenyEvaluator` requires a file path at construction time; there is no in-memory construction path.
- **Threshold configuration**: A window duration long enough that all submitted test events fall within a single epoch, with a count threshold that will be crossed during the run.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Both tests compile and pass when built with the `duckdb` preset; neither is compiled without it.
- **SC-002**: After each test completes (pass or fail), neither the temporary DuckDB file nor the temporary deny-list file remains on disk.
- **SC-003**: The benchmark test produces a non-zero p99 value in Catch2 benchmark output, with at least some `Flag` and `Block` decisions present in the collected set.
- **SC-004**: The load test passes its P99 ≤ 500ms assertion for both the overall distribution and all 10 per-tenant distributions.
- **SC-005**: At least one `Flag` verdict and at least one `Block` verdict appear in each test's collected decisions, confirming evaluators acted on event content.
- **SC-006**: The total decisions collected in the load test equals the total events submitted (30 000).

## Clarifications

### Session 2026-03-29

- Q: Acceptance Scenario 1 stated threshold = 500; research.md Decision 5 chose 200 — which value is canonical? → A: 200 (research.md is authoritative; 500 events for high-volume entity crosses 200 comfortably without excessive CAS contention)
- Q: Key Entities described the deny list as "embedded in the test (not loaded from a separate file)" — but `AllowDenyEvaluator` only accepts a `std::filesystem::path`. Is the deny list written to a temp file? → A: Yes — written to a temp file at test startup, cleaned up alongside the DuckDB file; Key Entities corrected accordingly.

## Assumptions

- DuckDB temp files (not in-memory `db_path = ""`) are used intentionally to exercise real on-disk persistence and startup recovery, even though this adds I/O to the measurement; the 500ms P99 budget accounts for this.
- The 500ms P99 budget (vs. 300ms for no-ops) is a conservative test-environment estimate accounting for CI machine variability, DuckDB temp-file I/O, and concurrent load overhead — not a relaxation of the constitutional 300ms production limit. If these tests consistently measure P99 > 300ms under stable conditions, that signals a production pipeline constitution violation requiring WriteBackWindowStore hot-path optimisation, not a spec budget increase.
- `AllowDenyEvaluator` will be initialized with a deny-list file written to a temp path by the test and cleaned up alongside the DuckDB file, since the evaluator loads lists from disk at construction time.
- The ONNX inference stage uses the same no-op stub pattern as existing benchmarks; no model file is needed.
- Both new tests run under the same `duckdb` CMake preset used by `test_write_back_pipeline.cpp`.
