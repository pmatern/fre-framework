#pragma once

/// Service harness public API for the fre-service binary.
///
/// The harness exposes three HTTP/1.1 endpoints over a TCP socket:
///
///   POST /events            — Submit a single JSON event; returns 202 on success
///   GET  /health            — Returns pipeline state and degradation flag as JSON
///   POST /pipeline/drain    — Drains the pipeline and stops accepting events
///
/// The HTTP server is hand-rolled (no Boost.Beast dependency) using Asio's
/// raw TCP acceptor and blocking read/write per-connection. This keeps the
/// dependency footprint minimal per constitution Principle III.

#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <thread>

namespace fre::service {

// ─── Config ───────────────────────────────────────────────────────────────────

struct HarnessConfig {
    std::string    bind_address{"127.0.0.1"};
    uint16_t       port{8080};
    std::string    pipeline_config_path;  ///< Path to JSON config file
    PipelineConfig pipeline_config;       ///< Used if pipeline_config_path is empty
};

// ─── ServiceHarness ──────────────────────────────────────────────────────────

class ServiceHarness {
public:
    explicit ServiceHarness(HarnessConfig config);
    ~ServiceHarness();

    ServiceHarness(const ServiceHarness&)            = delete;
    ServiceHarness& operator=(const ServiceHarness&) = delete;

    /// Start the HTTP listener and the pipeline.
    /// Returns false if the pipeline cannot be started.
    bool start();

    /// Block until stop() is called externally.
    void run();

    /// Drain the pipeline and stop accepting requests.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Load a PipelineConfig from a JSON file.
/// Returns nullopt on parse error (error message written to stderr).
std::optional<PipelineConfig>
load_config_from_json(const std::string& path);

}  // namespace fre::service
