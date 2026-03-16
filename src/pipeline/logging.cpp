#include <fre/core/logging.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>
#include <quill/sinks/RotatingFileSink.h>

#include <atomic>
#include <mutex>

namespace fre {

namespace {

std::once_flag   g_init_flag;
quill::Logger*   g_diagnostic_logger{nullptr};
quill::Logger*   g_audit_logger{nullptr};

}  // namespace

void init_logging(const LogConfig& config) {
    std::call_once(g_init_flag, [&config] {
        quill::BackendOptions backend_opts;
        quill::Backend::start(backend_opts);

        // ─── Diagnostic logger ───────────────────────────────────────────────
        auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");

        if (!config.diagnostic_file_path.empty()) {
            quill::RotatingFileSinkConfig rot_cfg;
            rot_cfg.set_max_backup_files(config.rotate_file_count);
            rot_cfg.set_rotation_max_file_size(config.rotate_max_bytes);
            auto file_sink = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
                config.diagnostic_file_path, rot_cfg);
            g_diagnostic_logger = quill::Frontend::create_or_get_logger(
                "fre.diagnostic", {console_sink, file_sink});
        } else {
            g_diagnostic_logger = quill::Frontend::create_or_get_logger(
                "fre.diagnostic", console_sink);
        }

        // ─── Audit logger ────────────────────────────────────────────────────
        if (!config.audit_file_path.empty()) {
            quill::FileSinkConfig audit_cfg;
            auto audit_sink = quill::Frontend::create_or_get_sink<quill::FileSink>(
                config.audit_file_path, audit_cfg);
            g_audit_logger = quill::Frontend::create_or_get_logger("fre.audit",
                                                                     {std::move(audit_sink)});
        } else {
            g_audit_logger = g_diagnostic_logger;
        }
    });
}

void flush_logging() {
    if (g_diagnostic_logger) {
        g_diagnostic_logger->flush_log();
    }
}

namespace detail {

quill::Logger* diagnostic_logger() noexcept { return g_diagnostic_logger; }
quill::Logger* audit_logger() noexcept { return g_audit_logger; }

}  // namespace detail

void log_audit(const Decision& decision) {
    if (!g_audit_logger) return;

    // Build NDJSON record inline (hot path: format args encoded in SPSC ring buffer)
    const auto final_v = [&] {
        switch (decision.final_verdict) {
            case Verdict::Pass:  return "pass";
            case Verdict::Flag:  return "flag";
            case Verdict::Block: return "block";
        }
        return "unknown";
    }();

    LOG_INFO(g_audit_logger,
        R"({{"event_id":{},"tenant_id":"{}","entity_id":"{}","final_verdict":"{}","degraded":{},"elapsed_us":{}}})",
        decision.event_id,
        decision.tenant_id,
        decision.entity_id,
        final_v,
        decision.is_degraded(),
        decision.elapsed_us);
}

}  // namespace fre
