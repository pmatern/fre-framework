#include <fre/evaluator/allow_deny_evaluator.hpp>
#include <fre/core/logging.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

namespace fre {

// ─── Helpers ─────────────────────────────────────────────────────────────────

std::unordered_set<std::string>
AllowDenyEvaluator::load_list(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }

    std::ifstream ifs{path};
    if (!ifs.is_open()) {
        throw std::runtime_error{
            "AllowDenyEvaluator: cannot open list file: " + path.string()};
    }

    std::unordered_set<std::string> result;
    std::string line;
    while (std::getline(ifs, line)) {
        // Strip trailing carriage return (Windows line endings)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            result.insert(std::move(line));
        }
    }
    return result;
}

// ─── Constructor ─────────────────────────────────────────────────────────────

AllowDenyEvaluator::AllowDenyEvaluator(AllowDenyEvaluatorConfig config)
    : config_{std::move(config)}
    , allow_set_{load_list(config_.allow_list_path)}
    , deny_set_{load_list(config_.deny_list_path)}
{
    FRE_LOG_INFO("AllowDenyEvaluator '{}' loaded: {} allow entries, {} deny entries",
                 config_.evaluator_id, allow_set_.size(), deny_set_.size());
}

// ─── match_key ───────────────────────────────────────────────────────────────

std::string_view AllowDenyEvaluator::match_key(const Event& event) const noexcept {
    switch (config_.match_field) {
        case AllowDenyMatchField::EntityId:
            return event.entity_id;
        case AllowDenyMatchField::TenantId:
            return event.tenant_id;
    }
    return event.entity_id;
}

// ─── evaluate ────────────────────────────────────────────────────────────────

std::expected<EvaluatorResult, EvaluatorError>
AllowDenyEvaluator::evaluate(const Event& event) const {
    const std::string key{match_key(event)};

    EvaluatorResult result;
    result.evaluator_id = std::string{config_.evaluator_id};

    if (!allow_set_.empty() && allow_set_.contains(key)) {
        result.verdict     = Verdict::Pass;
        result.reason_code = "allow_list_match";
        return result;
    }

    if (deny_set_.contains(key)) {
        result.verdict     = Verdict::Block;
        result.reason_code = "deny_list_match";
        return result;
    }

    result.verdict     = config_.default_verdict;
    result.reason_code = "default";
    return result;
}

}  // namespace fre
