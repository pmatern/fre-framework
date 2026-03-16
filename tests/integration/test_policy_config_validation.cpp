#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/policy/rule_engine.hpp>
#include <fre/stage/emit_stage.hpp>

#include <catch2/catch_test_macros.hpp>

// ─── Tests ───────────────────────────────────────────────────────────────────

SCENARIO("Pipeline start() returns ConfigError when policy rule references undefined stage",
         "[integration][policy][config]")
{
    GIVEN("a policy rule referencing StageId 'ml_inference' on a pipeline with no InferenceStage") {
        using namespace fre::policy;

        // A simple emission target (stdout)
        fre::EmitStageConfig emit_cfg;
        emit_cfg.add_stdout_target();

        // Policy rule references the 'inference' stage, but no InferenceStageConfig is set
        fre::PolicyStageConfig policy_cfg;
        policy_cfg.add_rule(
            PolicyRule{StageVerdictIs{"inference", fre::Verdict::Flag}},
            /*priority=*/ 1,
            /*action_verdict=*/ fre::Verdict::Block,
            /*rule_id=*/ "rule_referencing_absent_stage");

        auto config_result = fre::PipelineConfig::Builder{}
            .pipeline_id("test-config-validation")
            .policy_config(std::move(policy_cfg))
            .emit_config(std::move(emit_cfg))
            .build();

        WHEN("the config is built and the pipeline is started") {
            // build() may succeed (validation deferred to start()) or fail immediately.
            // Either is acceptable per spec — we assert that the error surfaces before events flow.
            bool error_detected = false;

            if (!config_result.has_value()) {
                // build() caught it early
                error_detected = true;
                const auto& err = config_result.error();
                THEN("ConfigError::UndefinedStageDependency is returned by build()") {
                    // Verify the error encodes the undefined stage reference
                    std::string msg = fre::error_message(err);
                    REQUIRE(msg.find("inference") != std::string::npos);
                    error_detected = true;
                }
            } else {
                fre::Pipeline pipeline{std::move(*config_result)};
                auto start_result = pipeline.start();
                if (!start_result.has_value()) {
                    error_detected = true;
                    THEN("ConfigError::UndefinedStageDependency is returned by start()") {
                        std::string msg = fre::error_message(start_result.error());
                        REQUIRE(msg.find("inference") != std::string::npos);
                    }
                }
            }

            THEN("the error was detected at build() or start() time") {
                REQUIRE(error_detected == true);
            }
        }
    }
}
