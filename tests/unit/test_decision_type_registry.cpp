#include <fre/core/decision_type.hpp>
#include <fre/core/decision.hpp>
#include <fre/core/verdict.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fre;

// ─── DecisionTypeRegistry ────────────────────────────────────────────────────

SCENARIO("DecisionTypeRegistry: add_type validates uniqueness and id", "[unit][decision_type]")
{
    GIVEN("an empty registry")
    {
        DecisionTypeRegistry reg;

        WHEN("a type with an empty id is added")
        {
            auto result = reg.add_type({"", "Empty", 0});
            THEN("it returns an error") { REQUIRE_FALSE(result.has_value()); }
        }

        WHEN("two types with distinct ids are added")
        {
            REQUIRE(reg.add_type({"block", "Block", 0}).has_value());
            REQUIRE(reg.add_type({"notify", "Notify", 50}).has_value());

            THEN("both are findable")
            {
                REQUIRE(reg.find("block")  != nullptr);
                REQUIRE(reg.find("notify") != nullptr);
                REQUIRE(reg.find("unknown") == nullptr);
            }

            AND_THEN("types() returns them sorted by priority ascending")
            {
                auto types = reg.types();
                REQUIRE(types.size() == 2);
                REQUIRE(types[0].id == "block");
                REQUIRE(types[1].id == "notify");
            }
        }

        WHEN("the same id is registered twice")
        {
            REQUIRE(reg.add_type({"block", "Block", 0}).has_value());
            auto dup = reg.add_type({"block", "Also Block", 10});
            THEN("the second registration returns an error") { REQUIRE_FALSE(dup.has_value()); }
        }
    }
}

SCENARIO("DecisionTypeRegistry: add_incompatible validates both ids", "[unit][decision_type]")
{
    GIVEN("a registry with two types")
    {
        DecisionTypeRegistry reg;
        REQUIRE(reg.add_type({"pass",  "Pass",  200}).has_value());
        REQUIRE(reg.add_type({"block", "Block", 0  }).has_value());

        WHEN("an incompatible pair is added for two registered types")
        {
            REQUIRE(reg.add_incompatible("pass", "block").has_value());

            THEN("are_incompatible returns true in both orders")
            {
                REQUIRE(reg.are_incompatible("pass",  "block"));
                REQUIRE(reg.are_incompatible("block", "pass"));
            }

            AND_THEN("unrelated types are not incompatible")
            {
                REQUIRE_FALSE(reg.are_incompatible("pass", "pass"));
            }
        }

        WHEN("an incompatible pair references an unregistered type")
        {
            auto result = reg.add_incompatible("block", "unknown");
            THEN("it returns an error") { REQUIRE_FALSE(result.has_value()); }
        }
    }
}

// ─── Decision::compute_active_decisions ──────────────────────────────────────

namespace {

// Build a Decision that has a policy stage output containing EvaluatorResults
// with the given (evaluator_id, decision_type_id) pairs.
Decision make_decision_with_policy(
    std::initializer_list<std::pair<std::string, std::string>> results)
{
    Decision d;
    StageOutput policy_out;
    policy_out.stage_id = "policy";

    for (auto& [eval_id, type_id] : results) {
        EvaluatorResult er;
        er.evaluator_id     = eval_id;
        er.verdict          = Verdict::Block;
        er.decision_type_id = type_id;
        policy_out.evaluator_results.push_back(std::move(er));
    }

    d.stage_outputs.push_back(std::move(policy_out));
    return d;
}

}  // namespace

SCENARIO("Decision::compute_active_decisions with nullptr registry is a no-op",
         "[unit][decision_type]")
{
    GIVEN("a decision with a policy stage result")
    {
        auto dec = make_decision_with_policy({{"rule1", "block"}});
        WHEN("compute_active_decisions is called with nullptr")
        {
            dec.compute_active_decisions(nullptr);
            THEN("active_decisions remains empty")
            {
                REQUIRE(dec.active_decisions.empty());
            }
        }
    }
}

SCENARIO("Decision::compute_active_decisions populates and sorts active decisions",
         "[unit][decision_type]")
{
    GIVEN("a registry with block(priority=0) and notify(priority=50)")
    {
        DecisionTypeRegistry reg;
        REQUIRE(reg.add_type({"block",  "Block",  0 }).has_value());
        REQUIRE(reg.add_type({"notify", "Notify", 50}).has_value());

        AND_GIVEN("a decision where both types fired")
        {
            auto dec = make_decision_with_policy({
                {"rule_block",  "block"},
                {"rule_notify", "notify"},
            });

            WHEN("compute_active_decisions is called")
            {
                dec.compute_active_decisions(&reg);

                THEN("two active decisions are produced, sorted by priority")
                {
                    REQUIRE(dec.active_decisions.size() == 2);
                    REQUIRE(dec.active_decisions[0].decision_type_id == "block");
                    REQUIRE(dec.active_decisions[0].priority == 0);
                    REQUIRE(dec.active_decisions[1].decision_type_id == "notify");
                    REQUIRE(dec.active_decisions[1].priority == 50);
                }

                AND_THEN("rule_ids are preserved")
                {
                    REQUIRE(dec.active_decisions[0].rule_id == "rule_block");
                    REQUIRE(dec.active_decisions[1].rule_id == "rule_notify");
                }
            }
        }
    }
}

SCENARIO("Decision::compute_active_decisions skips unknown decision_type_ids",
         "[unit][decision_type]")
{
    GIVEN("a registry with only 'block' registered")
    {
        DecisionTypeRegistry reg;
        REQUIRE(reg.add_type({"block", "Block", 0}).has_value());

        AND_GIVEN("a decision where 'notify' also fired but is not registered")
        {
            auto dec = make_decision_with_policy({
                {"rule_block",  "block"},
                {"rule_notify", "notify"},  // not in registry
            });

            WHEN("compute_active_decisions is called")
            {
                dec.compute_active_decisions(&reg);

                THEN("only the registered type appears in active_decisions")
                {
                    REQUIRE(dec.active_decisions.size() == 1);
                    REQUIRE(dec.active_decisions[0].decision_type_id == "block");
                }
            }
        }
    }
}

SCENARIO("Decision::compute_active_decisions ignores results without decision_type_id",
         "[unit][decision_type]")
{
    GIVEN("a registry and a decision mixing typed and untyped results")
    {
        DecisionTypeRegistry reg;
        REQUIRE(reg.add_type({"block", "Block", 0}).has_value());

        Decision dec;
        StageOutput policy_out;
        policy_out.stage_id = "policy";

        // Typed result
        EvaluatorResult typed;
        typed.evaluator_id     = "rule_block";
        typed.verdict          = Verdict::Block;
        typed.decision_type_id = "block";
        policy_out.evaluator_results.push_back(typed);

        // Untyped result (legacy)
        EvaluatorResult untyped;
        untyped.evaluator_id = "legacy_rule";
        untyped.verdict      = Verdict::Flag;
        // decision_type_id left as nullopt
        policy_out.evaluator_results.push_back(untyped);

        dec.stage_outputs.push_back(std::move(policy_out));

        WHEN("compute_active_decisions is called")
        {
            dec.compute_active_decisions(&reg);

            THEN("only the typed result appears in active_decisions")
            {
                REQUIRE(dec.active_decisions.size() == 1);
                REQUIRE(dec.active_decisions[0].decision_type_id == "block");
            }
        }
    }
}
