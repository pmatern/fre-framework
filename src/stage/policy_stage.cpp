#include <fre/stage/policy_stage.hpp>
#include <fre/core/logging.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace fre {

PolicyStage::PolicyStage(PolicyStageConfig config, const DecisionTypeRegistry* registry)
    : config_{std::move(config)}, registry_{registry} {}

std::expected<StageOutput, Error> PolicyStage::process(const PolicyContext& ctx) {
    const auto start = std::chrono::steady_clock::now();

    StageOutput out;
    out.stage_id = std::string{stage_id()};
    out.verdict  = Verdict::Pass;

    // Determine whether to use multi-decision path:
    // requires a registry AND at least one rule with a decision_type_id.
    const bool use_multi = (registry_ != nullptr) &&
        std::ranges::any_of(config_.rules,
            [](const PolicyStageRule& psr) { return psr.decision_type_id.has_value(); });

    if (!use_multi) {
        // ── Legacy path: first-match-wins ─────────────────────────────────────
        for (const PolicyStageRule& psr : config_.rules) {
            bool matched = false;
            try {
                matched = policy::RuleEngine::evaluate(ctx, psr.rule);
            } catch (...) {
                EvaluatorResult degraded;
                degraded.evaluator_id = psr.rule_id.empty() ? "unknown_rule" : psr.rule_id;
                degraded.verdict      = (config_.failure_mode == FailureMode::FailClosed)
                                            ? Verdict::Block : Verdict::Pass;
                degraded.skipped      = true;
                degraded.reason_code  = "rule_eval_exception";
                out.evaluator_results.push_back(std::move(degraded));
                out.degraded_reason |= DegradedReason::EvaluatorError;
                continue;
            }

            if (matched) {
                EvaluatorResult er;
                er.evaluator_id = psr.rule_id.empty() ? "policy_rule" : psr.rule_id;
                er.verdict      = psr.action_verdict;
                er.reason_code  = psr.rule_id;

                out.verdict = max_verdict(out.verdict, psr.action_verdict);
                out.evaluator_results.push_back(std::move(er));

                // First matching rule wins; stop evaluating further rules.
                break;
            }
        }
    } else {
        // ── Multi-decision path: evaluate all rules ───────────────────────────
        // Collect (decision_type_id, EvaluatorResult) for each matched rule
        // that carries a decision_type_id. Rules without one fall through to
        // the legacy verdict accumulation below.
        struct Match {
            std::string    type_id;
            uint32_t       priority{0};
            EvaluatorResult result;
        };
        std::vector<Match> matches;

        for (const PolicyStageRule& psr : config_.rules) {
            bool matched = false;
            try {
                matched = policy::RuleEngine::evaluate(ctx, psr.rule);
            } catch (...) {
                EvaluatorResult degraded;
                degraded.evaluator_id = psr.rule_id.empty() ? "unknown_rule" : psr.rule_id;
                degraded.verdict      = (config_.failure_mode == FailureMode::FailClosed)
                                            ? Verdict::Block : Verdict::Pass;
                degraded.skipped      = true;
                degraded.reason_code  = "rule_eval_exception";
                out.evaluator_results.push_back(std::move(degraded));
                out.degraded_reason |= DegradedReason::EvaluatorError;
                continue;
            }

            if (!matched) continue;

            if (!psr.decision_type_id.has_value()) {
                // Legacy rule in a multi-decision pipeline: contribute to verdict only.
                EvaluatorResult er;
                er.evaluator_id = psr.rule_id.empty() ? "policy_rule" : psr.rule_id;
                er.verdict      = psr.action_verdict;
                er.reason_code  = psr.rule_id;
                out.verdict = max_verdict(out.verdict, psr.action_verdict);
                out.evaluator_results.push_back(std::move(er));
                continue;
            }

            const auto* desc = registry_->find(*psr.decision_type_id);
            if (!desc) continue;  // not registered — skip (caught by Builder::validate)

            EvaluatorResult er;
            er.evaluator_id     = psr.rule_id.empty() ? "policy_rule" : psr.rule_id;
            er.verdict          = psr.action_verdict;
            er.reason_code      = psr.rule_id;
            er.decision_type_id = *psr.decision_type_id;

            matches.push_back(Match{*psr.decision_type_id, desc->priority, std::move(er)});
        }

        // Deduplicate: same decision_type_id → keep the first occurrence.
        // Rules are already sorted by ascending priority, so the first match for a
        // given type is the highest-precedence rule.
        std::vector<Match> deduped;
        deduped.reserve(matches.size());
        for (auto& match : matches) {
            const bool already = std::ranges::any_of(deduped,
                [&match](const Match& existing) { return existing.type_id == match.type_id; });
            if (!already) deduped.push_back(std::move(match));
        }

        // Apply combinability: for each incompatible pair where both types fired,
        // mark the lower-priority (higher priority number) one as a loser.
        // Collect losers first, then remove — avoids modifying while iterating.
        std::vector<std::string> losers;
        for (const auto& pair : registry_->incompatible_pairs()) {
            const bool has_a = std::ranges::any_of(deduped,
                [&pair](const Match& m) { return m.type_id == pair.type_id_a; });
            const bool has_b = std::ranges::any_of(deduped,
                [&pair](const Match& m) { return m.type_id == pair.type_id_b; });
            if (!has_a || !has_b) continue;

            const auto* desc_a = registry_->find(pair.type_id_a);
            const auto* desc_b = registry_->find(pair.type_id_b);
            if (!desc_a || !desc_b) continue;

            // Lower priority number = higher precedence = survives.
            // On equal priority, type_a survives (insertion-order tie-break).
            if (desc_a->priority <= desc_b->priority) {
                losers.push_back(pair.type_id_b);
            } else {
                losers.push_back(pair.type_id_a);
            }
        }

        // Emit one EvaluatorResult per surviving decision type.
        for (auto& match : deduped) {
            const bool is_loser = std::ranges::any_of(losers,
                [&match](const std::string& loser) { return loser == match.type_id; });
            if (is_loser) continue;

            out.verdict = max_verdict(out.verdict, match.result.verdict);
            out.evaluator_results.push_back(std::move(match.result));
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    out.elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());

    return out;
}

}  // namespace fre
