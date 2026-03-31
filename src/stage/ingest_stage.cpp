#include <fre/stage/ingest_stage.hpp>
#include <fre/core/logging.hpp>

#include <chrono>
#include <string>

namespace fre {

IngestStage::IngestStage(IngestStageConfig config) : config_{std::move(config)} {}

std::expected<StageOutput, Error> IngestStage::process(const Event& event) {
    StageOutput out;
    out.stage_id = std::string{stage_id()};

    const auto start = std::chrono::steady_clock::now();

    // ─── Validation ───────────────────────────────────────────────────────────

    if (!event.is_valid()) {
        out.verdict         = Verdict::Pass;  // EmitDegraded — pipeline still emits
        out.degraded_reason = DegradedReason::IngestValidationFailed;
        out.evaluator_results.push_back(EvaluatorResult{
            .evaluator_id = "ingest_validator",
            .verdict      = Verdict::Pass,
            .reason_code  = "invalid_event_fields",
            .score        = {},
            .skipped      = false,
            .metadata     = {},
        });

        const auto elapsed = std::chrono::steady_clock::now() - start;
        out.elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        return out;
    }

    // ─── Late arrival check ───────────────────────────────────────────────────

    const auto now       = std::chrono::system_clock::now();
    const auto event_age = std::chrono::duration_cast<std::chrono::milliseconds>(now - event.timestamp);

    if (event_age > config_.skew_tolerance) {
        FRE_LOG_WARNING("Late event: tenant={} entity={} age_ms={}",
                        event.tenant_id, event.entity_id, event_age.count());
        out.evaluator_results.push_back(EvaluatorResult{
            .evaluator_id = "ingest_skew_check",
            .verdict      = Verdict::Flag,
            .reason_code  = "late_arrival",
            .score        = {},
            .metadata     = {},
        });
        out.verdict = Verdict::Flag;
    } else {
        out.evaluator_results.push_back(EvaluatorResult{
            .evaluator_id = "ingest_validator",
            .verdict      = Verdict::Pass,
            .reason_code  = {},
            .score        = {},
            .metadata     = {},
        });
        out.verdict = Verdict::Pass;
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    out.elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    return out;
}

}  // namespace fre
