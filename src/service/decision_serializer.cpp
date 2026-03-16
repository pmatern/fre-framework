#include <fre/service/decision_serializer.hpp>

#include <nlohmann/json.hpp>

#include <chrono>

namespace fre::service {

using json = nlohmann::json;

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace {

std::string verdict_to_str(Verdict v) {
    switch (v) {
        case Verdict::Pass:  return "pass";
        case Verdict::Flag:  return "flag";
        case Verdict::Block: return "block";
    }
    return "pass";
}

Verdict str_to_verdict(std::string_view s) {
    if (s == "flag")  return Verdict::Flag;
    if (s == "block") return Verdict::Block;
    return Verdict::Pass;
}

json evaluator_result_to_json(const EvaluatorResult& r) {
    json j;
    j["evaluator_id"] = r.evaluator_id;
    j["verdict"]      = verdict_to_str(r.verdict);
    if (r.reason_code.has_value()) j["reason_code"] = *r.reason_code;
    j["skipped"]      = r.skipped;
    if (r.score.has_value()) j["score"] = *r.score;
    return j;
}

json stage_output_to_json(const StageOutput& so) {
    json j;
    j["stage_id"]    = so.stage_id;
    j["verdict"]     = verdict_to_str(so.verdict);
    j["elapsed_us"]  = so.elapsed_us;
    j["degraded"]    = static_cast<uint32_t>(so.degraded_reason);

    json results_arr = json::array();
    for (const auto& er : so.evaluator_results) {
        results_arr.push_back(evaluator_result_to_json(er));
    }
    j["evaluator_results"] = std::move(results_arr);
    return j;
}

}  // namespace

// ─── to_json ─────────────────────────────────────────────────────────────────

std::string DecisionSerializer::to_json(const Decision& d) {
    json j;
    j["pipeline_id"]      = d.pipeline_id;
    j["pipeline_version"] = d.pipeline_version;
    j["event_id"]         = d.event_id;
    j["tenant_id"]        = d.tenant_id;
    j["entity_id"]        = d.entity_id;
    j["final_verdict"]    = verdict_to_str(d.final_verdict);
    j["elapsed_us"]       = d.elapsed_us;
    j["degraded_reason"]  = static_cast<uint32_t>(d.degraded_reason);
    j["decided_at_us"]    = std::chrono::duration_cast<std::chrono::microseconds>(
                                d.decided_at.time_since_epoch()).count();

    json stages_arr = json::array();
    for (const auto& so : d.stage_outputs) {
        stages_arr.push_back(stage_output_to_json(so));
    }
    j["stage_outputs"] = std::move(stages_arr);

    return j.dump();
}

// ─── from_json ───────────────────────────────────────────────────────────────

std::expected<Decision, Error> DecisionSerializer::from_json(std::string_view json_str) {
    try {
        auto j = json::parse(json_str);

        Decision d;
        d.pipeline_id      = j.value("pipeline_id",      "");
        d.pipeline_version = j.value("pipeline_version", "");
        d.event_id         = j.value("event_id",         uint64_t{0});
        d.tenant_id        = j.value("tenant_id",        "");
        d.entity_id        = j.value("entity_id",        "");
        d.final_verdict    = str_to_verdict(j.value("final_verdict", "pass"));
        d.elapsed_us       = j.value("elapsed_us",       uint64_t{0});

        if (j.contains("stage_outputs")) {
            for (const auto& sj : j["stage_outputs"]) {
                StageOutput so;
                so.stage_id    = sj.value("stage_id", "");
                so.verdict     = str_to_verdict(sj.value("verdict", "pass"));
                so.elapsed_us  = sj.value("elapsed_us", uint64_t{0});
                d.stage_outputs.push_back(std::move(so));
            }
        }

        return d;
    } catch (const json::exception& e) {
        return std::unexpected(Error{EmissionError{
            EmissionErrorCode::SerializationError,
            "deserializer",
            e.what()}});
    }
}

}  // namespace fre::service
