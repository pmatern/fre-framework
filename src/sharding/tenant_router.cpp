#include <fre/sharding/tenant_router.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fre {

namespace {

// ─── Combinatorial hash for K-of-N cell assignment ───────────────────────────
//
// For each i in [0, K), compute H_i(tenant_id, seed_i) % N, ensuring no duplicates.
// Based on AWS shuffle sharding (Vogels 2014).

constexpr uint32_t k_hash_seeds[] = {
    0x9e3779b9u, 0x6c62272eu, 0x94d049bbu, 0xe9546b25u,
    0x12e15e35u, 0x3b1d8f2bu, 0x7c9e4ab3u, 0x4f5a1c9fu,
};

[[nodiscard]] std::vector<uint32_t> assign_cells(
    std::string_view tenant_id, uint32_t num_cells, uint32_t cells_per_tenant) {
    std::vector<uint32_t> cells;
    cells.reserve(cells_per_tenant);

    const std::size_t h = std::hash<std::string_view>{}(tenant_id);

    for (uint32_t i = 0; cells.size() < cells_per_tenant; ++i) {
        const uint32_t seed   = (i < std::size(k_hash_seeds)) ? k_hash_seeds[i] : (i * 0x9e3779b9u);
        const uint32_t cell   = static_cast<uint32_t>((h ^ (seed + i)) % num_cells);

        // Ensure no duplicates
        if (std::find(cells.begin(), cells.end(), cell) == cells.end()) {
            cells.push_back(cell);
        }
        // Safety: if all N cells are assigned before K unique ones, stop.
        if (cells.size() == num_cells) break;
    }

    return cells;
}

// ─── Lock-free token bucket ──────────────────────────────────────────────────
//
// Fixed-point: tokens stored as int64 * TOKEN_SCALE (1024).
// Refill: lazy on try_acquire, capped at capacity.

constexpr int64_t TOKEN_SCALE = 1024;

struct TokenBucket {
    std::atomic<int64_t>  tokens;         // fixed-point
    std::atomic<uint64_t> last_refill_ns; // wall-clock nanoseconds
    int64_t               capacity_fp;
    int64_t               rate_fp_per_ns; // tokens * TOKEN_SCALE per nanosecond

    explicit TokenBucket(int64_t capacity_tokens, int64_t tokens_per_second)
        : tokens{capacity_tokens * TOKEN_SCALE}
        , last_refill_ns{static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count())}
        , capacity_fp{capacity_tokens * TOKEN_SCALE}
        , rate_fp_per_ns{tokens_per_second * TOKEN_SCALE / 1'000'000'000LL} {}

    [[nodiscard]] bool try_consume() noexcept {
        const uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());

        // Refill — CAS loop
        uint64_t prev_ns = last_refill_ns.load(std::memory_order_relaxed);
        while (true) {
            const uint64_t elapsed = now_ns - prev_ns;
            if (elapsed == 0) break;

            const int64_t add = static_cast<int64_t>(elapsed) * rate_fp_per_ns;
            if (add <= 0) break;

            if (last_refill_ns.compare_exchange_weak(
                    prev_ns, now_ns, std::memory_order_relaxed)) {
                // Clamp to capacity
                int64_t cur = tokens.load(std::memory_order_relaxed);
                while (true) {
                    const int64_t next = std::min(cur + add, capacity_fp);
                    if (tokens.compare_exchange_weak(
                            cur, next, std::memory_order_relaxed)) break;
                }
                break;
            }
        }

        // Consume one token
        int64_t cur = tokens.load(std::memory_order_relaxed);
        while (cur >= TOKEN_SCALE) {
            if (tokens.compare_exchange_weak(
                    cur, cur - TOKEN_SCALE, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }
};

// ─── Per-tenant state ─────────────────────────────────────────────────────────

struct TenantState {
    std::vector<uint32_t> cell_indices;
    TokenBucket           bucket;
    std::atomic<int32_t>  in_flight{0};
    int32_t               max_concurrent;

    TenantState(std::vector<uint32_t> cells, const RateLimitConfig& rate)
        : cell_indices{std::move(cells)}
        , bucket{rate.bucket_capacity, rate.tokens_per_second}
        , max_concurrent{rate.max_concurrent} {}
};

}  // namespace

// ─── TenantRouter::Impl ───────────────────────────────────────────────────────

struct TenantRouter::Impl {
    ShardingConfig   shard_cfg;
    RateLimitConfig  rate_cfg;
    asio::thread_pool pool;
    std::vector<std::unique_ptr<Strand>> cells;

    mutable std::mutex                               tenant_mutex;
    std::unordered_map<std::string, TenantState>    tenants;

    explicit Impl(ShardingConfig sc, RateLimitConfig rc)
        : shard_cfg{sc}
        , rate_cfg{rc}
        , pool{sc.thread_count == 0
                   ? static_cast<uint32_t>(std::thread::hardware_concurrency())
                   : sc.thread_count}
    {
        cells.reserve(sc.num_cells);
        for (uint32_t i = 0; i < sc.num_cells; ++i) {
            cells.emplace_back(std::make_unique<Strand>(
                asio::make_strand(pool.get_executor())));
        }
    }

    TenantState& get_or_create_tenant(std::string_view tenant_id) {
        const std::string key{tenant_id};
        std::lock_guard   lock{tenant_mutex};
        if (auto it = tenants.find(key); it != tenants.end()) {
            return it->second;
        }
        auto cell_indices = assign_cells(tenant_id, shard_cfg.num_cells, shard_cfg.cells_per_tenant);
        auto [it, _] = tenants.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(std::move(cell_indices), rate_cfg));
        return it->second;
    }
};

// ─── TenantRouter public API ──────────────────────────────────────────────────

TenantRouter::TenantRouter(ShardingConfig shard_cfg, RateLimitConfig rate_cfg)
    : impl_{std::make_unique<Impl>(shard_cfg, rate_cfg)} {}

TenantRouter::~TenantRouter() { stop(); }

std::span<TenantRouter::Strand* const> TenantRouter::cells_for(
    std::string_view tenant_id) const noexcept {
    TenantState& ts = const_cast<Impl*>(impl_.get())->get_or_create_tenant(tenant_id);
    // Build a temporary view of Strand pointers.
    // NOTE: This returns pointers into the stable `cells` vector — safe as long as
    //       TenantRouter outlives the returned span.
    thread_local std::vector<Strand*> cell_ptrs;
    cell_ptrs.clear();
    for (uint32_t idx : ts.cell_indices) {
        cell_ptrs.push_back(impl_->cells[idx].get());
    }
    return std::span<Strand* const>{cell_ptrs};
}

std::expected<void, RateLimitError> TenantRouter::try_acquire(
    std::string_view tenant_id) noexcept {
    TenantState& ts = impl_->get_or_create_tenant(tenant_id);

    if (!ts.bucket.try_consume()) {
        return std::unexpected(RateLimitError{
            .code      = RateLimitErrorCode::Exhausted,
            .tenant_id = std::string{tenant_id},
        });
    }

    const int32_t prev = ts.in_flight.fetch_add(1, std::memory_order_acquire);
    if (prev >= ts.max_concurrent) {
        ts.in_flight.fetch_sub(1, std::memory_order_release);
        return std::unexpected(RateLimitError{
            .code      = RateLimitErrorCode::ConcurrencyCapReached,
            .tenant_id = std::string{tenant_id},
        });
    }

    return {};
}

void TenantRouter::release(std::string_view tenant_id) noexcept {
    std::lock_guard lock{impl_->tenant_mutex};
    if (auto it = impl_->tenants.find(std::string{tenant_id}); it != impl_->tenants.end()) {
        it->second.in_flight.fetch_sub(1, std::memory_order_release);
    }
}

void TenantRouter::stop() {
    impl_->pool.stop();
    impl_->pool.join();
}

TenantRouter::Executor TenantRouter::executor() noexcept {
    return impl_->pool.get_executor();
}

}  // namespace fre
