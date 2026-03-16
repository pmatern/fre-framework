/// Redis ExternalWindowStore adapter.
///
/// This file provides a Redis-backed implementation of the ExternalStoreBackend
/// function table declared in include/fre/state/external_store.hpp.
///
/// Dependencies:
///   - hiredis (C client) or redis-plus-plus (C++ wrapper)
///   - Enabled when CMake option FRE_ENABLE_REDIS=ON
///
/// The implementation below is a skeleton that satisfies the StateStore concept
/// interface. Full hiredis integration requires FRE_ENABLE_REDIS=ON.

#ifdef FRE_ENABLE_REDIS

#include <fre/state/external_store.hpp>
#include <fre/core/logging.hpp>

// #include <hiredis/hiredis.h>   // uncomment when FRE_ENABLE_REDIS=ON

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace fre {

/// Build an ExternalStoreBackend that delegates to a Redis instance.
///
/// @param host       Redis host (e.g. "127.0.0.1")
/// @param port       Redis port (default 6379)
/// @param ttl_ms     Window key TTL in milliseconds (passed to PEXPIRE)
///
/// The backend uses:
///   GET  key          → get()
///   SET  key value PX ttl NX  with optimistic version check → compare_and_swap()
///   DEL  key          → expire()
///   PING (cached 100ms)        → is_available()
ExternalStoreBackend make_redis_backend(const std::string& host,
                                        uint16_t port    = 6379,
                                        uint64_t ttl_ms  = 60'000)
{
    // TODO: connect via hiredis and implement each function-table slot.
    // Reference implementation:
    //
    //   ctx = redisConnect(host.c_str(), port);
    //
    //   backend.get = [ctx, ttl_ms](const WindowKey& key, void*) {
    //       auto reply = redisCommand(ctx, "GET %s", key.to_string().c_str());
    //       if (!reply || reply->type != REDIS_REPLY_STRING)
    //           return unexpected(StoreError{StoreErrorCode::Unavailable, "get failed"});
    //       return WindowValue::from_bytes(reply->str, reply->len);
    //   };
    //
    //   backend.compare_and_swap = [ctx, ttl_ms](const WindowKey& key,
    //       const WindowValue& expected, const WindowValue& desired, void*) {
    //       // Use MULTI/EXEC with WATCH for optimistic locking.
    //       // Or use a Lua script for atomic CAS semantics.
    //       ...
    //   };
    //
    //   backend.expire = [ctx](const WindowKey& key, void*) {
    //       redisCommand(ctx, "DEL %s", key.to_string().c_str());
    //       return {};
    //   };
    //
    //   backend.is_available = [ctx](void*) -> bool {
    //       auto reply = redisCommand(ctx, "PING");
    //       return reply && reply->type == REDIS_REPLY_STATUS;
    //   };
    (void)host;
    (void)port;
    (void)ttl_ms;

    // Stub: always unavailable — causes ExternalWindowStore to fall back to in-process.
    ExternalStoreBackend backend{};
    backend.is_available = [](void*) -> bool { return false; };
    backend.ctx = nullptr;

    FRE_LOG_WARNING("make_redis_backend: stub — hiredis integration not yet implemented");
    return backend;
}

}  // namespace fre

#endif  // FRE_ENABLE_REDIS
