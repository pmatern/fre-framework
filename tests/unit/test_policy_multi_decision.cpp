/// Unit tests for PolicyStage::process() multi-decision path.
/// Tests the stage in isolation (no full pipeline) to verify all-rules collection,
/// deduplication, combinability conflict resolution, legacy fallback,
/// and equal-priority tie-breaking.

#include <fre/core/decision_type.hpp>
#include <fre/core/event.hpp>
#include <fre/core/verdict.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/policy/rule_engine.hpp>
#include <fre/stage/policy_stage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using namespace fre;
using namespace fre::policy;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static const Event k_login_event{
    .tenant_id  = "tenant",
    .entity_id  = "entity",
    .event_type = "login",
    .timestamp  = std::chrono::system_clock::time_point{},
};

static PolicyContext make_ctx() {
    return PolicyContext{k_login_event, {}};
}

// Build a minimal registry with block(0), notify(50), pass(200).
static DecisionTypeRegistry make_registry() {
    DecisionTypeRegistry reg;
    [[maybe_unused]] auto r1 = reg.add_type({"block",  "Block",  0  });
    [[maybe_unused]] auto r2 = reg.add_type({"notify", "Notify", 50 });
    [[maybe_unused]] auto r3 = reg.add_type({"pass",   "Pass",   200});
    return reg;
}

// ─── Legacy path ──────────────────────────────────────────────────────────────

TEST_CASE("PolicyStage legacy: first-match-wins when no registry is provided",
          "[unit][policy_stage][multi_decision]")
{
    PolicyStageConfig cfg;
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 1, Verdict::Flag,  "flag_first");
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 2, Verdict::Block, "block_second");

    PolicyStage stage{cfg};  // no registry → legacy mode
    auto out = stage.process(make_ctx());

    REQUIRE(out.has_value());
    REQUIRE(out->verdict == Verdict::Flag);
    REQUIRE(out->evaluator_results.size() == 1);
    REQUIRE(out->evaluator_results[0].evaluator_id == "flag_first");
}

TEST_CASE("PolicyStage legacy: no match leaves verdict as Pass",
          "[unit][policy_stage][multi_decision]")
{
    PolicyStageConfig cfg;
    cfg.add_rule(PolicyRule{EventTypeIs{"payment"}}, 1, Verdict::Block, "block_payment");

    PolicyStage stage{cfg};
    auto out = stage.process(make_ctx());

    REQUIRE(out.has_value());
    REQUIRE(out->verdict == Verdict::Pass);
    REQUIRE(out->evaluator_results.empty());
}

// ─── Multi-decision: all-rules collection ────────────────────────────────────

TEST_CASE("PolicyStage multi: all matching rules are collected when registry is present",
          "[unit][policy_stage][multi_decision]")
{
    auto reg = make_registry();

    PolicyStageConfig cfg;
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 1, Verdict::Block, "r_block",  "block");
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 2, Verdict::Flag,  "r_notify", "notify");
    cfg.add_rule(PolicyRule{EventTypeIs{"other"}}, 3, Verdict::Pass,  "r_pass",   "pass");
    // third rule does NOT match (event_type is "login")

    PolicyStage stage{cfg, &reg};
    auto out = stage.process(make_ctx());

    REQUIRE(out.has_value());
    // block and notify matched; pass did not
    REQUIRE(out->evaluator_results.size() == 2);

    // verdict is max of surviving results
    REQUIRE(out->verdict == Verdict::Block);

    // both decision_type_ids are set
    REQUIRE(out->evaluator_results[0].decision_type_id.has_value());
    REQUIRE(out->evaluator_results[1].decision_type_id.has_value());

    const auto& ids = out->evaluator_results;
    bool has_block  = ids[0].decision_type_id == "block"  || ids[1].decision_type_id == "block";
    bool has_notify = ids[0].decision_type_id == "notify" || ids[1].decision_type_id == "notify";
    REQUIRE(has_block);
    REQUIRE(has_notify);
}

// ─── Multi-decision: deduplication ───────────────────────────────────────────

TEST_CASE("PolicyStage multi: same decision_type_id deduped — first rule wins",
          "[unit][policy_stage][multi_decision]")
{
    auto reg = make_registry();

    PolicyStageConfig cfg;
    // Two rules emit "block"; priority=1 fires first, should survive dedup.
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 1, Verdict::Block, "block_high", "block");
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 2, Verdict::Block, "block_low",  "block");

    PolicyStage stage{cfg, &reg};
    auto out = stage.process(make_ctx());

    REQUIRE(out.has_value());
    REQUIRE(out->evaluator_results.size() == 1);
    REQUIRE(out->evaluator_results[0].evaluator_id == "block_high");
    REQUIRE(out->evaluator_results[0].decision_type_id == "block");
}

// ─── Multi-decision: combinability conflict resolution ────────────────────────

TEST_CASE("PolicyStage multi: incompatible pair — higher-precedence type survives",
          "[unit][policy_stage][multi_decision]")
{
    DecisionTypeRegistry reg;
    REQUIRE(reg.add_type({"block", "Block", 0  }).has_value());
    REQUIRE(reg.add_type({"pass",  "Pass",  200}).has_value());
    REQUIRE(reg.add_incompatible("pass", "block").has_value());

    PolicyStageConfig cfg;
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 1, Verdict::Block, "r_block", "block");
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 2, Verdict::Pass,  "r_pass",  "pass");

    PolicyStage stage{cfg, &reg};
    auto out = stage.process(make_ctx());

    REQUIRE(out.has_value());
    // pass is incompatible with block; block has lower priority number (0 < 200) → block wins
    REQUIRE(out->evaluator_results.size() == 1);
    REQUIRE(out->evaluator_results[0].decision_type_id == "block");
    REQUIRE(out->verdict == Verdict::Block);
}

TEST_CASE("PolicyStage multi: compatible types both survive",
          "[unit][policy_stage][multi_decision]")
{
    DecisionTypeRegistry reg;
    REQUIRE(reg.add_type({"block",  "Block",  0 }).has_value());
    REQUIRE(reg.add_type({"notify", "Notify", 50}).has_value());
    // block and notify are NOT declared incompatible

    PolicyStageConfig cfg;
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 1, Verdict::Block, "r_block",  "block");
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 2, Verdict::Flag,  "r_notify", "notify");

    PolicyStage stage{cfg, &reg};
    auto out = stage.process(make_ctx());

    REQUIRE(out.has_value());
    REQUIRE(out->evaluator_results.size() == 2);
    REQUIRE(out->verdict == Verdict::Block);
}

// ─── Multi-decision: equal-priority tie-break ─────────────────────────────────

TEST_CASE("PolicyStage multi: equal-priority incompatible pair — type_id_a in pair survives",
          "[unit][policy_stage][multi_decision]")
{
    DecisionTypeRegistry reg;
    // Same priority: both are 10
    REQUIRE(reg.add_type({"alpha", "Alpha", 10}).has_value());
    REQUIRE(reg.add_type({"beta",  "Beta",  10}).has_value());
    // declare alpha,beta incompatible — alpha is type_id_a → alpha survives on tie
    REQUIRE(reg.add_incompatible("alpha", "beta").has_value());

    PolicyStageConfig cfg;
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 1, Verdict::Flag, "r_alpha", "alpha");
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 2, Verdict::Flag, "r_beta",  "beta");

    PolicyStage stage{cfg, &reg};
    auto out = stage.process(make_ctx());

    REQUIRE(out.has_value());
    REQUIRE(out->evaluator_results.size() == 1);
    // alpha is type_id_a in the pair → survives on equal priority
    REQUIRE(out->evaluator_results[0].decision_type_id == "alpha");
}

// ─── Multi-decision: legacy rules coexist with typed rules ───────────────────

TEST_CASE("PolicyStage multi: legacy rule (no decision_type_id) contributes to verdict only",
          "[unit][policy_stage][multi_decision]")
{
    auto reg = make_registry();

    PolicyStageConfig cfg;
    // Legacy rule: no decision_type_id — contributes to verdict but not to active_decisions
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 1, Verdict::Flag, "legacy_flag");
    // Typed rule: contributes to active_decisions
    cfg.add_rule(PolicyRule{EventTypeIs{"login"}}, 2, Verdict::Block, "typed_block", "block");

    PolicyStage stage{cfg, &reg};
    auto out = stage.process(make_ctx());

    REQUIRE(out.has_value());
    REQUIRE(out->evaluator_results.size() == 2);

    // Legacy result has no decision_type_id
    const auto* legacy = [&]() -> const EvaluatorResult* {
        for (const auto& er : out->evaluator_results) {
            if (er.evaluator_id == "legacy_flag") return &er;
        }
        return nullptr;
    }();
    REQUIRE(legacy != nullptr);
    REQUIRE_FALSE(legacy->decision_type_id.has_value());

    // Typed result has decision_type_id
    const auto* typed = [&]() -> const EvaluatorResult* {
        for (const auto& er : out->evaluator_results) {
            if (er.evaluator_id == "typed_block") return &er;
        }
        return nullptr;
    }();
    REQUIRE(typed != nullptr);
    REQUIRE(typed->decision_type_id == "block");

    // Verdict accounts for both
    REQUIRE(out->verdict == Verdict::Block);
}
