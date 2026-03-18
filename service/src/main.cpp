/// fre-service entry point.
/// The service harness implementation lives in harness.cpp / harness.hpp.

#include <fre/service/harness.hpp>
#include <fre/service/fleet_router.hpp>
#include <fre/core/logging.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace {

/// Load fleet topology from a JSON file: [{\"id\": 0, \"address\": \"host:port\"}, ...]
std::vector<fre::service::InstanceInfo> load_topology(const char* path) {
    std::vector<fre::service::InstanceInfo> result;
    if (!path || path[0] == '\0') return result;

    std::ifstream ifs{path};
    if (!ifs.is_open()) {
        std::cerr << "[fre-service] cannot open topology file: " << path << "\n";
        return result;
    }

    try {
        auto arr = nlohmann::json::parse(ifs);
        for (const auto& entry : arr) {
            fre::service::InstanceInfo info;
            info.id      = entry.value("id",      0u);
            info.address = entry.value("address", "");
            result.push_back(std::move(info));
        }
    } catch (const nlohmann::json::parse_error& ex) {
        std::cerr << "[fre-service] topology parse error: " << ex.what() << "\n";
    }
    return result;
}

}  // namespace

int main(int argc, char* argv[]) {
    fre::LogConfig log_cfg;
    log_cfg.diagnostic_level = fre::LogLevel::Info;
    fre::init_logging(log_cfg);

    fre::service::HarnessConfig harness_cfg;
    harness_cfg.bind_address = "127.0.0.1";
    harness_cfg.port         = 8080;

    // ── Fleet sharding config (optional) ──────────────────────────────────────
    const char* env_instance_id = std::getenv("FRE_INSTANCE_ID");
    const char* env_fleet_size  = std::getenv("FRE_FLEET_SIZE");
    if (env_instance_id && env_fleet_size) {
        fre::service::FleetConfig fleet;
        fleet.instance_id          = static_cast<uint32_t>(std::stoul(env_instance_id));
        fleet.fleet_size           = static_cast<uint32_t>(std::stoul(env_fleet_size));
        const char* env_ips        = std::getenv("FRE_INSTANCES_PER_TENANT");
        if (env_ips) {
            fleet.instances_per_tenant = static_cast<uint32_t>(std::stoul(env_ips));
        }
        fleet.topology = load_topology(std::getenv("FRE_TOPOLOGY_FILE"));
        harness_cfg.fleet_config = std::move(fleet);
        std::cout << "[fre-service] fleet mode: instance " << fleet.instance_id
                  << " of " << fleet.fleet_size << "\n";
    }

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
