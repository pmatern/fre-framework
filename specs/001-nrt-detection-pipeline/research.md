# Research: Near Real-Time Detection Pipeline Framework

**Feature**: 001-nrt-detection-pipeline
**Date**: 2026-03-15
**Status**: Complete — all NEEDS CLARIFICATION resolved

---

## 1. Build System

**Decision**: CMake 3.28+ with presets, CPM.cmake as the primary dependency manager, with
`install()` + `export()` targets so the library is consumable via `find_package()`. vcpkg
manifest mode as an optional complement for heavy system libraries.

**Rationale**: CMake is the de facto standard for C++ library interoperability. CMake 3.28
adds native C++20 module support and improved preset chaining, which cleanly separates CI
presets from developer presets. CPM.cmake wraps `FetchContent` with version-pinning hashes
and `CPM_SOURCE_CACHE` for reproducible builds without requiring a pre-installed package
manager. Properly written `install(EXPORT)` targets let downstream consumers use
`find_package(fre)` without knowing how the project was built.

**Alternatives considered**:
- **Bazel**: Best-in-class for monorepos and remote caching, but C++ toolchain setup is
  difficult on macOS/Windows and downstream CMake consumers cannot trivially consume it.
- **Meson**: Clean and fast; rejected because its CMake compatibility layer is incomplete
  and there is no first-class equivalent of `install(EXPORT)` generating relocatable targets.
- **Conan 2.x**: Superior ABI-aware binary caching but requires Conan installed on every
  developer and CI node — unacceptable friction for a library meant for broad consumption.
- **FetchContent alone**: Lacks version-pinning hashes and `CPM_SOURCE_CACHE`; CPM adds
  meaningful safety with minimal overhead.

---

## 2. Coroutine Executor / Scheduler

**Decision**: Standalone Asio 1.30+ as the coroutine executor. Use `asio::awaitable<T>`
for coroutine tasks and `asio::strand<thread_pool::executor_type>` as the per-cell
executor primitive. Wrap coroutine returns in a bespoke `Task<T, E>` adapter that surfaces
errors as `std::expected<T, E>` — no exceptions anywhere. Cancellation via
`asio::cancel_after` + `RunOptions::SetTerminate()` for ML inference timeouts.

**Rationale**: Standalone Asio (not Boost.Asio) is header-only, production-hardened, and
has had stable C++20 coroutine support since v1.19. Its `asio::strand` primitive gives free
serialization within a tenant's execution context without explicit locking — this is exactly
the primitive needed for shuffle-sharded worker cells. `asio::cancel_after` maps directly
to per-stage timeout enforcement. `use_awaitable_t<Allocator>` allows custom allocators for
coroutine frame placement, enabling arena allocation on the hot path.

**Alternatives considered**:
- **stdexec / P2300**: The right long-term answer (C++26), but API has undergone breaking
  changes throughout 2024 and there is no stable ABI yet. Revisit for C++26.
- **libunifex**: Effectively abandoned in favor of stdexec. Rejected.
- **cppcoro**: Abandoned (last meaningful commit 2020), known MSVC bugs. Rejected.
- **Custom executor**: Reinventing strand semantics, cancellation propagation, and executor
  concept compliance is months of non-differentiating work. Asio passes these tests already.

---

## 3. ONNX Runtime C++ API

**Decision**: ONNX Runtime 1.19.x, dynamically linked. Use the C API (`OrtApi`) at all
ABI boundaries (plugin contracts); use the header-only C++ wrapper
(`onnxruntime_cxx_api.h`) for ergonomics within the main binary. One `Ort::Session` per
model, shared across coroutine workers (thread-safe for concurrent `Run()` calls). Enforce
per-inference timeouts via watchdog `std::jthread` + `RunOptions::SetTerminate()`.

**Rationale**: Dynamic linking is strongly preferred: the static library is 70–150 MB
depending on enabled execution providers (EPs), EPs themselves are dynamically loaded and
cannot function correctly from a fully static build, and ONNX Runtime's own documentation
warns against static linking with multiple EPs. Thread-safety: `Ort::Env` is
process-global and thread-safe after init; `Ort::Session::Run()` is thread-safe for
concurrent calls. Timeout mechanism: set terminate on `RunOptions` from a watchdog thread;
`Run()` returns `OrtStatus*` with code `ORT_FAIL` (not an exception) once the intra-op
thread polls the flag — effective granularity ~1–5 ms.

**Alternatives considered**:
- **Static linking**: Rejected — too large, incompatible with EP plugins.
- **TensorFlow Lite / OpenVINO**: Platform-specific optimisation wins but ONNX Runtime
  provides a single API surface across all platforms. Rejected as primary.
- **libtorch**: Very large footprint, slow build times, licensing complexity. Rejected.

---

## 4. ABI-Stable Plugin Contract

**Decision**: Define the plugin contract as a pure C API boundary (`extern "C"` struct
of function pointers — a C vtable). Provide a C++23 concept (`EvaluatorConcept`) and a
`CppEvaluatorAdapter<Impl>` wrapper template for same-binary use. Dynamic library plugins
implement the C vtable and are loaded via `dlopen`/`LoadLibrary`.

**Rationale**: C++ has no stable ABI across compiler vendors, versions, or stdlib
implementations. Name mangling, vtable layout, `std::string` layout, and exception tables
all vary. The C vtable pattern (a struct of `extern "C"` function pointers + an opaque
`void* ctx`) is universally portable. Embed a `uint32_t version` field as the first member;
hosts check the version at load time and treat unknown fields as null. For same-binary use
(where ABI stability is not a concern), `CppEvaluatorAdapter<Impl>` fills the C vtable at
construction time — single dispatch path for both cases.

**Alternatives considered**:
- **Pure-virtual C++ base class**: Fragile across compilers and stdlib versions; impossible
  to use safely from a plugin compiled with a different standard library. Rejected.
- **COM/IUnknown style**: Excellent stability and versioning model; over-engineered for a
  focused framework. Rejected as primary.
- **C++23 modules**: Do not solve ABI stability — same ABI as non-module equivalents.

---

## 5. Windowed Aggregation State

**Decision**: In-process default: power-of-two ring buffer indexed by window epoch for
tumbling windows; extended ring buffer (N = width/step slots) for sliding windows. Expiry
driven by a hierarchical time-wheel (Hashed Timing Wheel). External backend: pluggable
`StateStore` concept with a Redis adapter (using redis-plus-plus / hiredis) as the
reference optional implementation.

**Rationale**: Ring buffer indexed by `epoch = floor(timestamp / window_size)` gives O(1)
insert and O(1) window read with cache-friendly access. A hierarchical time-wheel
(per Kafka Streams and Netty) gives O(1) amortized insert and expiry. For session windows,
a doubly-linked expiry list with a min-heap is appropriate. The external backend uses the
`StateStore` concept (CAS-based, as specified in `contracts/state-store-contract.md`) so
the ring buffer and Redis adapter are interchangeable behind the same interface.

**Alternatives considered**:
- **Segment tree / Fenwick tree**: Appropriate for arbitrary range queries; overkill for
  fixed-size windows. Rejected as primary.
- **SQLite in-process**: Write latency under concurrent access too high for sub-10ms
  event processing. Rejected.
- **Apache Arrow in-process batch accumulation**: Batch-first model; does not fit
  per-event streaming. Rejected.

---

## 6. Shuffle Sharding for In-Process Multi-Tenant Isolation

**Decision**: N=16 worker cells (Asio strands on a shared thread pool), K=4 cells per
tenant. Tenant-to-cell assignment via deterministic combinatorial hashing:
`H_i(tenant_id, seed_i)` for i in [0, K), each selecting a cell from [0, N) without
replacement. Fixed N at startup — no consistent hashing needed. Work-stealing DISABLED on
the isolation pool.

**Rationale**: AWS shuffle sharding (Vogels 2014): with K=4, N=16, any two tenants share
all 4 cells with probability C(4,4)/C(16,4) ≈ 0.07% — near-zero blast-radius overlap.
One saturated tenant affects at most K/N = 25% of cells; the remaining 75% serve others
unaffected. Asio strands are the ideal cell primitive: they serialize work within a cell
without a dedicated thread, and the shared thread pool reclaims idle capacity across cells.
Work-stealing is disabled on the isolation pool because it would allow a noisy tenant to
capture work slots outside its assigned cells, defeating the isolation guarantee.

**Alternatives considered**:
- **Single shared pool**: Zero isolation. Rejected.
- **One thread per tenant**: Unacceptable for hundreds of tenants. Rejected.
- **Consistent hashing ring**: Correct for dynamic node sets; over-engineered for fixed N.
  Acceptable future extension if dynamic cell scaling is needed.

---

## 7. Per-Tenant Noisy-Neighbor Mitigation

**Decision**: Lock-free token bucket per tenant using `std::atomic<int64_t>` (tokens,
fixed-point) + `std::atomic<uint64_t>` (last-refill timestamp), CAS-loop update.
Combined with a per-tenant concurrency cap via `std::atomic<int>` RAII semaphore.
Back-pressure strategy: **Reject** (return `std::unexpected(RateLimitError::Exhausted)`)
for immediate callers; bounded queue with deadline for async callers.

**Rationale**: Token bucket allows short bursts up to capacity while enforcing a long-run
average rate — appropriate for detection workloads that are bursty but bounded. Lock-free
CAS loop is typically 1–3 iterations under contention, giving sub-microsecond overhead.
Per-tenant concurrency cap (atomic semaphore) prevents slow evaluators from accumulating
unbounded in-flight requests. Reject back-pressure is lowest-latency and correct for hard
SLA contracts; it converts an overload condition into a trackable error rather than a
memory-exhaustion condition.

**Alternatives considered**:
- **Leaky bucket**: Enforces strict constant rate; too rigid for bursty-but-bounded
  workloads. Rejected as primary.
- **Sliding window counter with sub-buckets**: More accurate rate measurement; useful as
  audit/alerting complement but more complex to make lock-free.
- **OS cgroup / thread priority**: Only OS scheduler granularity (~1–10 ms); requires
  elevated privileges. Rejected.

---

## 8. Testing Framework

**Decision**: Catch2 v3 (3.6+) with `CATCH_CONFIG_FAST_COMPILE`. Use
`TEMPLATE_TEST_CASE` and `TEMPLATE_PRODUCT_TEST_CASE` for concept-parameterized tests.
Custom `IsExpected<T>` and `IsUnexpected<E>` Catch2 matchers for `std::expected` unwrapping.
Use `BENCHMARK` macro for in-framework latency regression tests.

**Rationale**: Catch2 v3's compiled form dramatically improves build times over v2's
single-header. BDD-style `GIVEN`/`WHEN`/`THEN` macros map naturally onto the pipeline's
event-in / decision-out test structure. `TEMPLATE_TEST_CASE` enables concept-satisfaction
regression tests over multiple concrete evaluator implementations. `BENCHMARK` avoids
needing a second benchmarking harness (e.g., Google Benchmark) for latency checks.

**Alternatives considered**:
- **GoogleTest**: Best-in-class mocking; rejected because gmock generates ABI-sensitive
  vtable stubs that conflict with no-RTTI goals, and SECTION-based decomposition is more
  expressive than TEST_F for pipeline state-machine testing.
- **doctest**: Near-identical API, faster compile; lacks `BENCHMARK` and has less active
  maintenance than Catch2 v3. Acceptable drop-in if compile time becomes critical.

---

## 9. Structured Logging

**Decision**: quill v4+ as the hot-path diagnostic logger (lock-free SPSC ring buffer,
backend thread does all formatting and I/O). Separate quill `Logger` with NDJSON
`FileHandler` for the structured audit trail (one JSON object per line). `{fmt}` as the
formatting engine for both.

**Rationale**: quill's architecture encodes format arguments into a SPSC ring buffer on
the calling thread (20–50 ns) and offloads all formatting/I/O to a backend thread —
compared to spdlog's ~200–500 ns hot path (which calls `fmt::format` inline by default).
For a pipeline with a 300 ms total budget and sub-10 ms per-stage allocations, the
difference between a 50 ns and 500 ns log call on the critical path is meaningful at scale.
The NDJSON audit sink enables streaming ingestion by log aggregation systems (Vector,
Fluent Bit, Elasticsearch) without a custom format.

**Alternatives considered**:
- **spdlog**: Widely used; good documentation; `{fmt}` integration. Hot path calls
  `fmt::format` inline — too slow for sub-millisecond stage budgets at scale. Acceptable
  for moderate-latency (>1ms) pipelines.
- **Custom `{fmt}`-based logger**: Maximum control; weeks of work reinventing async queue,
  sink management, and level filtering that quill already provides. Rejected.
- **Abseil logging**: Modern and structured; does not prioritize low-latency hot paths.
  Rejected.

---

## 10. Serialization for Decision Records

**Decision**: FlatBuffers for binary Decision records over the service harness network
boundary (zero-copy reads, no deserialization-to-heap, additive schema evolution).
`nlohmann/json` (or `{fmt}`-generated JSON strings) for human-readable REST API responses
and debug output only.

**Rationale**: FlatBuffers allows a received byte buffer to be accessed directly as a
typed object without a parse/deserialize pass — field access is a pointer dereference.
This eliminates per-decision heap allocation for the binary protocol path. Schema evolution
is additive (new fields appended to tables with defaults; old readers ignore unknown
fields), satisfying rolling-deployment requirements. FlatBuffers' generated C++ accessor
code is simple to audit; `flatc` integrates cleanly into CMake via
`FlatBuffers::flatbuffers` imported target.

**Alternatives considered**:
- **Cap'n Proto**: Also zero-copy; excellent schema evolution. Rejected as primary because
  the `libkj` runtime dependency is heavier to integrate and the community is smaller.
  Acceptable alternative if consumers already use Cap'n Proto.
- **Protobuf 3**: Industry standard; mandatory parse-to-heap step allocates memory
  proportional to message size — unacceptable for thousands of decisions per second.
  Acceptable for gRPC service harness transport if gRPC is adopted.
- **MessagePack**: Compact binary; requires parsing; no zero-copy. Rejected.
- **nlohmann/json for everything**: O(n) parse, heavy allocation; acceptable for
  configuration and REST, unacceptable for high-throughput binary records.
- **Apache Arrow IPC**: Designed for columnar batches; per-record overhead too high.
  Acceptable for bulk decision history export.
