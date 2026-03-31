#include <fre/core/logging.hpp>

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <mutex>

namespace fre {

namespace {

std::once_flag                    g_init_flag;
std::shared_ptr<spdlog::logger>   g_diagnostic_logger;
std::shared_ptr<spdlog::logger>   g_audit_logger;

spdlog::level::level_enum to_spdlog_level(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace:    return spdlog::level::trace;
        case LogLevel::Debug:    return spdlog::level::debug;
        case LogLevel::Info:     return spdlog::level::info;
        case LogLevel::Warning:  return spdlog::level::warn;
        case LogLevel::Error:    return spdlog::level::err;
        case LogLevel::Critical: return spdlog::level::critical;
    }
    return spdlog::level::info;
}

}  // namespace

void init_logging(const LogConfig& config) {
    std::call_once(g_init_flag, [&config] {
        // Async thread pool: 8192-entry queue, 1 background thread.
        spdlog::init_thread_pool(8192, 1);

        // ─── Diagnostic logger ───────────────────────────────────────────────
        std::vector<spdlog::sink_ptr> diag_sinks;

        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        diag_sinks.push_back(console_sink);

        if (!config.diagnostic_file_path.empty()) {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config.diagnostic_file_path,
                config.rotate_max_bytes,
                config.rotate_file_count);
            diag_sinks.push_back(std::move(file_sink));
        }

        g_diagnostic_logger = std::make_shared<spdlog::async_logger>(
            "fre.diagnostic",
            diag_sinks.begin(), diag_sinks.end(),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::overrun_oldest);
        g_diagnostic_logger->set_level(to_spdlog_level(config.diagnostic_level));
        spdlog::register_logger(g_diagnostic_logger);

        // ─── Audit logger ────────────────────────────────────────────────────
        if (!config.audit_file_path.empty()) {
            auto audit_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                config.audit_file_path, /*truncate=*/false);
            g_audit_logger = std::make_shared<spdlog::async_logger>(
                "fre.audit",
                std::move(audit_sink),
                spdlog::thread_pool(),
                spdlog::async_overflow_policy::overrun_oldest);
            spdlog::register_logger(g_audit_logger);
        } else {
            g_audit_logger = g_diagnostic_logger;
        }
    });
}

void flush_logging() {
    if (g_diagnostic_logger) {
        g_diagnostic_logger->flush();
    }
}

namespace detail {

std::shared_ptr<spdlog::logger> diagnostic_logger() noexcept { return g_diagnostic_logger; }
std::shared_ptr<spdlog::logger> audit_logger() noexcept { return g_audit_logger; }

}  // namespace detail

void log_audit(const Decision& decision) {
    if (!g_audit_logger) return;

    const auto final_v = [&] {
        switch (decision.final_verdict) {
            case Verdict::Pass:  return "pass";
            case Verdict::Flag:  return "flag";
            case Verdict::Block: return "block";
        }
        return "unknown";
    }();

    g_audit_logger->info(
        R"({{"event_id":{},"tenant_id":"{}","entity_id":"{}","final_verdict":"{}","degraded":{},"elapsed_us":{}}})",
        decision.event_id,
        decision.tenant_id,
        decision.entity_id,
        final_v,
        decision.is_degraded(),
        decision.elapsed_us);
}

}  // namespace fre
