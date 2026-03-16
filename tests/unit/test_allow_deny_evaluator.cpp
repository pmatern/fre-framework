#include <fre/evaluator/allow_deny_evaluator.hpp>
#include <fre/core/event.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::filesystem::path write_list_file(const std::string& filename,
                                             const std::string& content)
{
    auto path = std::filesystem::temp_directory_path() / filename;
    std::ofstream ofs{path};
    ofs << content;
    return path;
}

static fre::Event make_event(std::string_view entity_id,
                              std::string_view tenant_id = "t")
{
    fre::Event e{};
    e.entity_id  = entity_id;
    e.tenant_id  = tenant_id;
    e.event_type = "test";
    return e;
}

// ─── Tests ───────────────────────────────────────────────────────────────────

SCENARIO("AllowDenyEvaluator with allow-list and deny-list files", "[unit][allow_deny]") {

    GIVEN("an allow-list containing entity-A and a deny-list containing entity-B") {
        auto allow_path = write_list_file("test_allow.txt", "entity-A\nentity-C\n");
        auto deny_path  = write_list_file("test_deny.txt",  "entity-B\n");

        fre::AllowDenyEvaluatorConfig cfg;
        cfg.allow_list_path = allow_path;
        cfg.deny_list_path  = deny_path;
        cfg.match_field     = fre::AllowDenyMatchField::EntityId;
        cfg.default_verdict = fre::Verdict::Pass;

        fre::AllowDenyEvaluator evaluator{cfg};

        WHEN("entity-A (on allow-list) is submitted") {
            auto result = evaluator.evaluate(make_event("entity-A"));
            THEN("result is Pass") {
                REQUIRE(result.has_value());
                REQUIRE(result->verdict == fre::Verdict::Pass);
            }
        }

        WHEN("entity-B (on deny-list) is submitted") {
            auto result = evaluator.evaluate(make_event("entity-B"));
            THEN("result is Block") {
                REQUIRE(result.has_value());
                REQUIRE(result->verdict == fre::Verdict::Block);
            }
        }

        WHEN("entity-Z (on neither list) is submitted") {
            auto result = evaluator.evaluate(make_event("entity-Z"));
            THEN("result is the configured default (Pass)") {
                REQUIRE(result.has_value());
                REQUIRE(result->verdict == fre::Verdict::Pass);
            }
        }

        WHEN("entity-Z with default=Flag is submitted") {
            fre::AllowDenyEvaluatorConfig cfg2 = cfg;
            cfg2.default_verdict = fre::Verdict::Flag;
            fre::AllowDenyEvaluator eval2{cfg2};

            auto result = eval2.evaluate(make_event("entity-Z"));
            THEN("result is Flag (configured default)") {
                REQUIRE(result.has_value());
                REQUIRE(result->verdict == fre::Verdict::Flag);
            }
        }
    }

    GIVEN("a missing allow-list file path") {
        fre::AllowDenyEvaluatorConfig cfg;
        cfg.allow_list_path = "/nonexistent/path/allow.txt";
        cfg.match_field     = fre::AllowDenyMatchField::EntityId;

        WHEN("AllowDenyEvaluator is constructed") {
            THEN("construction throws or evaluate returns EvaluatorError") {
                bool error_detected = false;
                try {
                    fre::AllowDenyEvaluator evaluator{cfg};
                    auto result = evaluator.evaluate(make_event("entity-A"));
                    if (!result.has_value()) {
                        error_detected = true;
                    }
                } catch (...) {
                    error_detected = true;
                }
                REQUIRE(error_detected == true);
            }
        }
    }

    GIVEN("match_field = TenantId") {
        auto deny_path = write_list_file("test_deny_tenant.txt", "bad-tenant\n");

        fre::AllowDenyEvaluatorConfig cfg;
        cfg.deny_list_path  = deny_path;
        cfg.match_field     = fre::AllowDenyMatchField::TenantId;
        cfg.default_verdict = fre::Verdict::Pass;

        fre::AllowDenyEvaluator evaluator{cfg};

        WHEN("event with tenant_id=bad-tenant is submitted") {
            fre::Event ev  = make_event("any-entity", "bad-tenant");
            auto result    = evaluator.evaluate(ev);
            THEN("result is Block") {
                REQUIRE(result.has_value());
                REQUIRE(result->verdict == fre::Verdict::Block);
            }
        }

        WHEN("event with tenant_id=ok-tenant is submitted") {
            fre::Event ev  = make_event("any-entity", "ok-tenant");
            auto result    = evaluator.evaluate(ev);
            THEN("result is Pass (default)") {
                REQUIRE(result.has_value());
                REQUIRE(result->verdict == fre::Verdict::Pass);
            }
        }
    }
}
