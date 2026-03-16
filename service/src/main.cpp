/// fre-service entry point.
/// The service harness implementation lives in harness.cpp / harness.hpp.

#include <fre/service/harness.hpp>
#include <fre/core/logging.hpp>

#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[]) {
    fre::LogConfig log_cfg;
    log_cfg.diagnostic_level = fre::LogLevel::Info;
    fre::init_logging(log_cfg);

    fre::service::HarnessConfig harness_cfg;
    harness_cfg.bind_address = "127.0.0.1";
    harness_cfg.port         = 8080;

    if (argc > 1) {
        harness_cfg.pipeline_config_path = argv[1];
    } else {
        // Default minimal config: stdout emit
        auto config_result = fre::PipelineConfig::Builder{}
            .pipeline_id("fre-service")
            .emit_config([]{
                fre::EmitStageConfig c;
                c.add_stdout_target();
                return c;
            }())
            .build();

        if (!config_result.has_value()) {
            std::cerr << "[fre-service] default config error: "
                      << fre::error_message(config_result.error()) << "\n";
            return EXIT_FAILURE;
        }
        harness_cfg.pipeline_config = std::move(*config_result);
    }

    fre::service::ServiceHarness harness{std::move(harness_cfg)};
    if (!harness.start()) {
        return EXIT_FAILURE;
    }
    harness.run();
    return EXIT_SUCCESS;
}
