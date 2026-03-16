#pragma once

#include <fre/core/error.hpp>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

namespace fre {

// ─── ShardingConfig ───────────────────────────────────────────────────────────

struct ShardingConfig {
    /// Total number of worker cells. Must be a power of two. Default: 16.
    uint32_t num_cells{16};

    /// Number of cells assigned per tenant. Must be < num_cells. Default: 4.
    uint32_t cells_per_tenant{4};

    /// Number of threads in the underlying thread pool. Default: hardware_concurrency.
    uint32_t thread_count{0};  // 0 = std::thread::hardware_concurrency()
};

// ─── RateLimitConfig ─────────────────────────────────────────────────────────

struct RateLimitConfig {
    /// Token bucket capacity (burst ceiling). Default: 1000 tokens.
    int64_t bucket_capacity{1000};

    /// Token refill rate (tokens per second). Default: 500.
    int64_t tokens_per_second{500};

    /// Maximum number of in-flight evaluations per tenant. Default: 100.
    int32_t max_concurrent{100};
};

// ─── TenantRouter ────────────────────────────────────────────────────────────

class TenantRouter {
public:
    using Executor = asio::thread_pool::executor_type;
    using Strand   = asio::strand<Executor>;

    explicit TenantRouter(ShardingConfig shard_cfg = {}, RateLimitConfig rate_cfg = {});
    ~TenantRouter();

    TenantRouter(const TenantRouter&)            = delete;
    TenantRouter& operator=(const TenantRouter&) = delete;
    TenantRouter(TenantRouter&&)                 = delete;
    TenantRouter& operator=(TenantRouter&&)      = delete;

    /// Returns a span of K strand references assigned to this tenant.
    /// Assignment is deterministic and stable for the lifetime of this router.
    [[nodiscard]] std::span<Strand* const> cells_for(std::string_view tenant_id) const noexcept;

    /// Attempt to acquire a rate-limit token and a concurrency slot for this tenant.
    /// Returns std::unexpected(RateLimitError) if either limit is exceeded.
    [[nodiscard]] std::expected<void, RateLimitError> try_acquire(
        std::string_view tenant_id) noexcept;

    /// Release the concurrency slot for this tenant (call when evaluation completes).
    void release(std::string_view tenant_id) noexcept;

    /// Stop accepting new work and drain the thread pool.
    void stop();

    /// Returns the underlying thread pool executor (use for non-tenant work).
    [[nodiscard]] Executor executor() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─── RAII concurrency guard ───────────────────────────────────────────────────

/// Calls router.release(tenant_id) on destruction.
class TenantConcurrencyGuard {
public:
    TenantConcurrencyGuard(TenantRouter& router, std::string_view tenant_id) noexcept
        : router_{router}, tenant_id_{tenant_id} {}

    ~TenantConcurrencyGuard() noexcept { router_.release(tenant_id_); }

    TenantConcurrencyGuard(const TenantConcurrencyGuard&)            = delete;
    TenantConcurrencyGuard& operator=(const TenantConcurrencyGuard&) = delete;

private:
    TenantRouter&    router_;
    std::string_view tenant_id_;
};

}  // namespace fre
