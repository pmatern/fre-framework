/// Multi-decision pipeline example: configurable decision types with exclusivity rules
///
/// Scenario: a payment fraud pipeline that can emit any combination of:
///
///   block   (priority 0)  — deny the transaction entirely
///   review  (priority 10) — flag for manual review
///   notify  (priority 20) — send an alert to the security team
///   pass    (priority 100) — explicit approval (incompatible with block/review)
///
/// Exclusivity rules:
///   pass ⊕ block  — an event cannot be both approved and blocked
///   pass ⊕ review — an event cannot be both approved and queued for review
///   notify is NOT restricted — alerts fire regardless of outcome.
///
/// Policy rules (evaluated against lightweight eval stage output):
///
///   priority 1 — eval verdict is Block → block + notify + review
///                (country_risk returned Block ⟹ AnyBlock composition elevates to Block)
///   priority 2 — eval verdict is Flag  → review + notify
///                (velocity alone, country was fine)
///   priority 3 — entity_class=known_good tag present → pass
///
/// Deduplication:
///   When both priority-1 and priority-2 rules fire (impossible — stage verdict is
///   either Flag or Block, not both), only one set fires. If only Block fires, the
///   priority-1 "notify" wins any later duplicate.
///
/// Scenarios:
///   A — low velocity, safe country      → eval=Pass  → no decisions
///   B — high velocity, safe country     → eval=Flag  → [review, notify]
///   C — high velocity, high-risk country→ eval=Block → [block, review, notify]
///   D — known-good + high velocity      → eval=Flag  → pass fires but is eliminated
///                                                       by pass⊕review → [review, notify]
///   E — known-good, low velocity        → eval=Pass  → [pass]

#include <fre/core/decision_type.hpp>
#include <fre/core/event.hpp>
#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/policy/rule_engine.hpp>

#include <chrono>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace fre;
using namespace fre::policy;
using namespace std::chrono_literals;

// ─── Evaluators ───────────────────────────────────────────────────────────────

// Flags if tx_count_1h tag exceeds 20.
struct VelocityEvaluator {
    std::expected<EvaluatorResult, EvaluatorError>
    evaluate(const Event& event) const {
        const auto count_str = event.tag("tx_count_1h");
        if (!count_str.empty() && std::stoi(std::string{count_str}) > 20) {
            return EvaluatorResult{
                .evaluator_id = "velocity",
                .verdict      = Verdict::Flag,
                .reason_code  = "velocity_exceeded",
            };
        }
        return EvaluatorResult{.evaluator_id = "velocity", .verdict = Verdict::Pass};
    }
};
static_assert(LightweightEvaluator<VelocityEvaluator>);

// Blocks if country tag is "XX" (sanctioned).
struct CountryRiskEvaluator {
    std::expected<EvaluatorResult, EvaluatorError>
    evaluate(const Event& event) const {
        if (event.tag("country") == "XX") {
            return EvaluatorResult{
                .evaluator_id = "country_risk",
                .verdict      = Verdict::Block,
                .reason_code  = "high_risk_country",
            };
        }
        return EvaluatorResult{.evaluator_id = "country_risk", .verdict = Verdict::Pass};
    }
};
static_assert(LightweightEvaluator<CountryRiskEvaluator>);

// ─── Emission target ──────────────────────────────────────────────────────────

struct PrintingTarget {
    std::expected<void, EmissionError> emit(Decision decision) {
        const char* verdict_str = "Pass";
        if (decision.final_verdict == Verdict::Flag)  verdict_str = "Flag";
        if (decision.final_verdict == Verdict::Block) verdict_str = "Block";

        std::cout << "\n── Decision ──────────────────────────────────────\n";
        std::cout << "  entity:  " << decision.entity_id << '\n';
        std::cout << "  verdict: " << verdict_str << '\n';

        if (decision.active_decisions.empty()) {
            std::cout << "  active:  (none — no policy rule fired)\n";
        } else {
            for (const auto& active : decision.active_decisions) {
                std::cout << "  active:  [p=" << active.priority << "] "
                          << active.decision_type_id
                          << "  ← " << active.rule_id << '\n';
            }
        }
        std::cout << "─────────────────────────────────────────────────\n";
        return {};
    }
};
static_assert(EmissionTarget<PrintingTarget>);

// ─── Decision type registry ───────────────────────────────────────────────────

DecisionTypeRegistry build_registry() {
    DecisionTypeRegistry reg;

    auto require = [](std::expected<void, ConfigError> result, const char* ctx) {
        if (!result) {
            std::cerr << ctx << ": " << result.error().detail << '\n';
            std::exit(1);
        }
    };

    // Lower priority number = higher precedence in conflict resolution.
    require(reg.add_type({"block",  "Block Transaction", 0  }), "add block");
    require(reg.add_type({"review", "Queue for Review",  10 }), "add review");
    require(reg.add_type({"notify", "Alert Security",    20 }), "add notify");
    require(reg.add_type({"pass",   "Explicit Approval", 100}), "add pass");

    // Exclusivity:
    //   pass cannot coexist with block  → block wins  (priority 0  < 100)
    //   pass cannot coexist with review → review wins (priority 10 < 100)
    //   notify has no restrictions: an alert is always desirable.
    require(reg.add_incompatible("pass", "block"),  "incompatible pass/block");
    require(reg.add_incompatible("pass", "review"), "incompatible pass/review");

    return reg;
}

// ─── Policy rules ─────────────────────────────────────────────────────────────
//
// Each rule carries a decision_type_id. All matching rules are evaluated,
// duplicates for the same type are removed (highest-precedence rule wins),
// and then incompatible pairs are resolved.
//
// Key observation: StageVerdictIs checks the composed stage verdict exactly.
// With AnyBlock composition:
//   - velocity=Flag, country=Pass  → eval verdict = Flag
//   - velocity=Flag, country=Block → eval verdict = Block  (Block subsumes Flag)
// So StageVerdictIs{"eval", Flag} and StageVerdictIs{"eval", Block} are mutually
// exclusive conditions — they cleanly separate the two risk levels.

PolicyStageConfig build_policy() {
    PolicyStageConfig cfg;

    // ── Priority 1: eval stage is Block (country_risk fired) ──────────────────
    // Emit block, review, and notify independently. All three are compatible so
    // all survive. "notify" from this priority will win deduplication over any
    // lower-priority rule that also emits "notify".
    cfg.add_rule(
        PolicyRule{StageVerdictIs{"eval", Verdict::Block}},
        1, Verdict::Block, "block_country_risk", "block");

    cfg.add_rule(
        PolicyRule{StageVerdictIs{"eval", Verdict::Block}},
        1, Verdict::Flag, "review_country_risk", "review");

    cfg.add_rule(
        PolicyRule{StageVerdictIs{"eval", Verdict::Block}},
        1, Verdict::Flag, "notify_country_risk", "notify");

    // ── Priority 2: eval stage is Flag (velocity alone, country was fine) ─────
    // Both rules emit different types so both survive initially.
    // "notify" is deduplicated against any priority-1 notify — the priority-1
    // version wins, so this one is dropped in that case.
    cfg.add_rule(
        PolicyRule{StageVerdictIs{"eval", Verdict::Flag}},
        2, Verdict::Flag, "review_velocity", "review");

    cfg.add_rule(
        PolicyRule{StageVerdictIs{"eval", Verdict::Flag}},
        2, Verdict::Flag, "notify_velocity", "notify");

    // ── Priority 3: explicit approval for known-good entities ─────────────────
    // Fires independently of eval verdict (the tag alone is enough).
    // If eval also flagged/blocked, pass will conflict with review/block and
    // be eliminated by combinability (lower priority number wins).
    cfg.add_rule(
        PolicyRule{TagEquals{"entity_class", "known_good"}},
        3, Verdict::Pass, "pass_known_good", "pass");

    return cfg;
}

// ─── Scenario runner ──────────────────────────────────────────────────────────

static void run_scenario(Pipeline& pipeline, const char* label,
                         std::vector<Tag> tags, const char* entity_id)
{
    std::cout << "\n=== " << label << " ===";

    Event ev;
    ev.tenant_id  = "acme-payments";
    ev.entity_id  = entity_id;
    ev.event_type = "payment";
    ev.timestamp  = std::chrono::system_clock::now();
    ev.tags       = std::span<const Tag>{tags};

    if (auto result = pipeline.submit(std::move(ev)); !result) {
        std::cerr << "submit error: " << error_message(result.error()) << '\n';
    }

    pipeline.drain(1000ms);

    // drain() stops the pipeline; restart for the next scenario.
    if (auto result = pipeline.start(); !result) {
        std::cerr << "restart error: " << error_message(result.error()) << '\n';
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    auto registry = build_registry();
    auto policy   = build_policy();

    // Note: add_target(T&&) is lvalue-qualified only; build EmitStageConfig
    // as a named variable rather than chaining off a temporary.
    EmitStageConfig emit_cfg;
    emit_cfg.add_target(PrintingTarget{});

    auto cfg_result = PipelineConfig::Builder{}
        .pipeline_id("payment-fraud-pipeline")
        .latency_budget(300ms)
        .lightweight_eval(
            EvalStageConfig{
                .timeout      = 10ms,
                .failure_mode = FailureMode::FailOpen,
                .composition  = CompositionRule::AnyBlock,
            }
            .add_evaluator(VelocityEvaluator{})
            .add_evaluator(CountryRiskEvaluator{})
        )
        .policy_config(std::move(policy))
        .decision_types(std::move(registry))   // ← opt-in to multi-decision mode
        .emit(std::move(emit_cfg))
        .build();

    if (!cfg_result) {
        std::cerr << "Config error: " << error_message(cfg_result.error()) << '\n';
        return 1;
    }

    Pipeline pipeline{std::move(*cfg_result)};

    if (auto result = pipeline.start(); !result) {
        std::cerr << "Start error: " << error_message(result.error()) << '\n';
        return 1;
    }

    // ── A: clean transaction ──────────────────────────────────────────────────
    // velocity=Pass, country=Pass → eval=Pass → no policy rule fires.
    // Expected: active_decisions empty.
    run_scenario(pipeline,
        "A: clean — expect no active decisions",
        {Tag{"tx_count_1h", "5"}, Tag{"country", "US"}},
        "clean-user");

    // ── B: high velocity, safe country ───────────────────────────────────────
    // velocity=Flag, country=Pass → eval=Flag (AnyBlock: no block ⟹ take Flag).
    // Rules at priority 2 fire: review_velocity + notify_velocity.
    // No conflict. Expected: [review(10), notify(20)].
    run_scenario(pipeline,
        "B: high velocity + safe country — expect [review, notify]",
        {Tag{"tx_count_1h", "25"}, Tag{"country", "US"}},
        "busy-user");

    // ── C: high velocity + high-risk country ─────────────────────────────────
    // velocity=Flag, country=Block → eval=Block (AnyBlock: Block subsumes Flag).
    // Rules at priority 1 fire: block_country_risk + review_country_risk + notify_country_risk.
    // Rules at priority 2 do NOT fire (StageVerdictIs{Flag} is false when verdict=Block).
    // No conflict among block/review/notify. Expected: [block(0), review(10), notify(20)].
    run_scenario(pipeline,
        "C: velocity + high-risk country — expect [block, review, notify]",
        {Tag{"tx_count_1h", "30"}, Tag{"country", "XX"}},
        "risky-user");

    // ── D: known-good entity + high velocity ─────────────────────────────────
    // velocity=Flag, country=Pass → eval=Flag.
    // Priority 2 fires: review + notify.
    // Priority 3 fires: pass.
    // Combinability: pass⊕review conflict → review(10) beats pass(100) → pass eliminated.
    // notify has no restrictions → survives.
    // Expected: [review(10), notify(20)]  (pass removed by combinability).
    run_scenario(pipeline,
        "D: known-good + high velocity — expect pass eliminated → [review, notify]",
        {Tag{"tx_count_1h", "25"}, Tag{"country", "US"}, Tag{"entity_class", "known_good"}},
        "trusted-but-busy");

    // ── E: known-good entity, low velocity ───────────────────────────────────
    // velocity=Pass, country=Pass → eval=Pass.
    // Only priority-3 rule fires: pass.
    // No incompatible counterpart fires → pass survives.
    // Expected: [pass(100)].
    run_scenario(pipeline,
        "E: known-good + low velocity — expect [pass]",
        {Tag{"tx_count_1h", "3"}, Tag{"country", "US"}, Tag{"entity_class", "known_good"}},
        "vip-user");

    // Final drain (run_scenario left the pipeline running after the last restart).
    pipeline.drain(1000ms);

    return 0;
}
