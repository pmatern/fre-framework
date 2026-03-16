#include <fre/pipeline/pipeline_config.hpp>
#include <fre/core/error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <variant>

using namespace fre;
using namespace std::chrono_literals;

// ─── Minimal evaluator and target for builder tests ──────────────────────────

struct NoopEvaluator {
    std::expected<EvaluatorResult, EvaluatorError> evaluate(const Event& /*ev*/) {
        return EvaluatorResult{.evaluator_id = "noop", .verdict = Verdict::Pass};
    }
};
static_assert(LightweightEvaluator<NoopEvaluator>);

struct NoopTarget {
    std::expected<void, EmissionError> emit(Decision /*d*/) { return {}; }
};
static_assert(EmissionTarget<NoopTarget>);

// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("PipelineConfig builder: valid config builds successfully", "[unit][pipeline_config][US1]") {
    auto target = std::make_shared<NoopTarget>();

    auto result = PipelineConfig::Builder()
        .pipeline_id("test-pipeline")
        .latency_budget(300ms)
        .ingest({})
        .lightweight_eval(
            EvalStageConfig{.timeout = 10ms, .failure_mode = FailureMode::FailOpen}
            .add_evaluator(NoopEvaluator{})
        )
        .emit(EmitStageConfig{.timeout = 10ms}.add_target(target))
        .build();

    REQUIRE(result.has_value());
    REQUIRE(result->pipeline_id == "test-pipeline");
}

TEST_CASE("PipelineConfig builder: RequiredStageMissing when emit absent", "[unit][pipeline_config][US1]") {
    auto result = PipelineConfig::Builder()
        .pipeline_id("no-emit")
        .latency_budget(300ms)
        .ingest({})
        .build();

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::holds_alternative<ConfigError>(result.error()));
    const auto& err = std::get<ConfigError>(result.error());
    CHECK(err.code == ConfigErrorCode::RequiredStageMissing);
}

TEST_CASE("PipelineConfig builder: LatencyBudgetExceeded when stage timeouts exceed budget", "[unit][pipeline_config][US1]") {
    auto target = std::make_shared<NoopTarget>();

    auto result = PipelineConfig::Builder()
        .pipeline_id("over-budget")
        .latency_budget(20ms)  // tiny budget
        .ingest(IngestStageConfig{.timeout = 10ms})
        .lightweight_eval(EvalStageConfig{.timeout = 10ms}.add_evaluator(NoopEvaluator{}))
        .emit(EmitStageConfig{.timeout = 10ms}.add_target(target))
        // ingest(10) + eval(10) + emit(10) = 30ms > 20ms budget
        .build();

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::holds_alternative<ConfigError>(result.error()));
    const auto& err = std::get<ConfigError>(result.error());
    CHECK(err.code == ConfigErrorCode::LatencyBudgetExceeded);
}

TEST_CASE("PipelineConfig builder: InvalidShardingConfig when K >= N", "[unit][pipeline_config][US1]") {
    auto target = std::make_shared<NoopTarget>();

    auto result = PipelineConfig::Builder()
        .pipeline_id("bad-sharding")
        .latency_budget(300ms)
        .ingest({})
        .emit(EmitStageConfig{.timeout = 10ms}.add_target(target))
        .sharding(ShardingConfig{.num_cells = 4, .cells_per_tenant = 4})  // K == N: invalid
        .build();

    REQUIRE_FALSE(result.has_value());
    REQUIRE(std::holds_alternative<ConfigError>(result.error()));
    const auto& err = std::get<ConfigError>(result.error());
    CHECK(err.code == ConfigErrorCode::InvalidShardingConfig);
}
