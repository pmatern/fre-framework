#pragma once

#include <fre/core/decision.hpp>

#include <spdlog/logger.h>

#include <cstdint>
#include <memory>
#include <string>

namespace fre {

// ─── LogLevel ────────────────────────────────────────────────────────────────

enum class LogLevel : uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

// ─── LogConfig ───────────────────────────────────────────────────────────────

struct LogConfig {
    /// Minimum diagnostic log level.
    LogLevel diagnostic_level{LogLevel::Info};

    /// Path for the rotating diagnostic log file. Empty = console only.
    std::string diagnostic_file_path;

    /// Path for the append-only NDJSON audit log. Empty = disabled.
    std::string audit_file_path;

    /// Maximum size (bytes) before rotating the diagnostic log file.
    uint64_t rotate_max_bytes{100 * 1024 * 1024};  // 100 MiB

    /// Number of rotated files to keep.
    uint32_t rotate_file_count{5};
};

// ─── Logging subsystem lifecycle ─────────────────────────────────────────────

/// Initialize the logging subsystem. Must be called once before any pipeline is started.
/// Thread-safe; subsequent calls are no-ops.
void init_logging(const LogConfig& config);

/// Flush all pending log records. Called during pipeline drain.
void flush_logging();

// ─── Internal accessor (do not call from user code) ──────────────────────────

namespace detail {
std::shared_ptr<spdlog::logger> diagnostic_logger() noexcept;
std::shared_ptr<spdlog::logger> audit_logger() noexcept;
}  // namespace detail

// ─── Audit record emission ───────────────────────────────────────────────────

/// Serialize a Decision to a single NDJSON line and write to the audit logger.
void log_audit(const Decision& decision);

}  // namespace fre

// ─── Diagnostic logging macros ───────────────────────────────────────────────
// Use these instead of calling spdlog directly so the logger handle is managed centrally.

#define FRE_LOG_TRACE(fmt, ...)   do { if (auto _l = ::fre::detail::diagnostic_logger()) _l->trace(fmt __VA_OPT__(,) __VA_ARGS__); } while(0)
#define FRE_LOG_DEBUG(fmt, ...)   do { if (auto _l = ::fre::detail::diagnostic_logger()) _l->debug(fmt __VA_OPT__(,) __VA_ARGS__); } while(0)
#define FRE_LOG_INFO(fmt, ...)    do { if (auto _l = ::fre::detail::diagnostic_logger()) _l->info(fmt __VA_OPT__(,) __VA_ARGS__); } while(0)
#define FRE_LOG_WARNING(fmt, ...) do { if (auto _l = ::fre::detail::diagnostic_logger()) _l->warn(fmt __VA_OPT__(,) __VA_ARGS__); } while(0)
#define FRE_LOG_ERROR(fmt, ...)   do { if (auto _l = ::fre::detail::diagnostic_logger()) _l->error(fmt __VA_OPT__(,) __VA_ARGS__); } while(0)
