#include <fre/stage/emit_stage.hpp>
#include <fre/core/logging.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace fre {

// ─── StdoutEmissionTarget ────────────────────────────────────────────────────

std::expected<void, EmissionError> StdoutEmissionTarget::emit(Decision d) {
    const char* verdict_str = [&] {
        switch (d.final_verdict) {
            case Verdict::Pass:  return "pass";
            case Verdict::Flag:  return "flag";
            case Verdict::Block: return "block";
        }
        return "unknown";
    }();
    std::cout << "[decision] event=" << d.event_id
              << " tenant=" << d.tenant_id
              << " entity=" << d.entity_id
              << " verdict=" << verdict_str
              << " elapsed_us=" << d.elapsed_us
              << " degraded=" << d.is_degraded()
              << '\n';
    return {};
}

// ─── EmitStage ───────────────────────────────────────────────────────────────

EmitStage::EmitStage(EmitStageConfig config) : config_{std::move(config)} {}

std::expected<StageOutput, Error> EmitStage::process(Decision decision) {
    StageOutput out;
    out.stage_id = std::string{stage_id()};

    const auto start = std::chrono::steady_clock::now();

    for (const auto& target_fn : config_.targets) {
        uint32_t attempts = 0;
        bool     succeeded = false;

        while (attempts <= config_.retry_limit) {
            auto result = target_fn(decision);
            if (result.has_value()) {
                succeeded = true;
                break;
            }
            ++attempts;

            if (attempts <= config_.retry_limit) {
                // Exponential back-off: 10ms, 20ms, 40ms…
                const auto delay = std::chrono::milliseconds{10 * (1 << (attempts - 1))};
                std::this_thread::sleep_for(delay);
            } else {
                FRE_LOG_ERROR("Emission target failed after {} retries: {}",
                              attempts, result.error().message());
                ++dropped_decisions_;
                out.degraded_reason |= DegradedReason::EmissionRetryExhausted;
            }
        }

        if (succeeded) {
            out.evaluator_results.push_back(EvaluatorResult{
                .evaluator_id = "emission_target",
                .verdict      = Verdict::Pass,
                .reason_code  = {},
                .score        = {},
                .metadata     = {},
            });
        }
    }

    out.verdict = Verdict::Pass;  // emission does not change final verdict

    const auto elapsed = std::chrono::steady_clock::now() - start;
    out.elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());

    return out;
}

}  // namespace fre
