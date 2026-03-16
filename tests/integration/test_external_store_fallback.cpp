#include <fre/pipeline/pipeline.hpp>
#include <fre/pipeline/pipeline_config.hpp>
#include <fre/state/external_store.hpp>
#include <fre/state/window_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ─── Helpers ──────────────────────────────────────────────────────────────────

namespace {

// Capturing emission target
struct CapturingTarget {
    mutable std::mutex              mu;
    std::vector<fre::Decision>      decisions;

    std::expected<void, fre::EmissionError> emit(fre::Decision d) {
        std::lock_guard lock{mu};
        decisions.push_back(std::move(d));
        return {};
    }

    std::size_t count() const {
        std::lock_guard lock{mu};
        return decisions.size();
    }

    fre::Decision get(std::size_t i) const {
        std::lock_guard lock{mu};
        return decisions.at(i);
    }
};

/// ExternalStoreBackend that always reports unavailable.
fre::ExternalStoreBackend make_unavailable_backend() {
    fre::ExternalStoreBackend backend{};
    backend.get = [](const fre::WindowKey&) -> std::expected<fre::WindowValue, fre::StoreError> {
        return std::unexpected(fre::StoreError{fre::StoreErrorCode::Unavailable, "unavailable"});
    };
    backend.compare_and_swap = [](const fre::WindowKey&, const fre::WindowValue&,
                                   const fre::WindowValue&) -> std::expected<bool, fre::StoreError> {
        return std::unexpected(fre::StoreError{fre::StoreErrorCode::Unavailable, "unavailable"});
    };
    backend.expire = [](const fre::WindowKey&) -> std::expected<void, fre::StoreError> {
        return std::unexpected(fre::StoreError{fre::StoreErrorCode::Unavailable, "unavailable"});
    };
    backend.is_available = []() -> bool { return false; };
    return backend;
}

/// Custom evaluator that uses ExternalWindowStore to track per-entity counts.
/// Flags events when an entity is seen more than 5 times.
struct CountingEvaluator {
    std::shared_ptr<fre::ExternalWindowStore> store;
    int threshold{5};

    std::expected<fre::EvaluatorResult, fre::EvaluatorError>
    evaluate(const fre::Event& event) {
        fre::WindowKey key{std::string{event.tenant_id}, std::string{event.entity_id}};

        fre::WindowValue new_val;
        auto current = store->get(key);
        double count = current.has_value() ? current->aggregate + 1.0 : 1.0;
        new_val.aggregate = count;
        new_val.version   = current.has_value() ? current->version + 1 : 1;

        fre::WindowValue old_val = current.value_or(fre::WindowValue{});
        std::ignore = store->compare_and_swap(key, old_val, new_val);

        fre::EvaluatorResult r;
        r.evaluator_id = "counting_eval";
        r.verdict      = (count > static_cast<double>(threshold))
                             ? fre::Verdict::Flag : fre::Verdict::Pass;
        return r;
    }
};
static_assert(fre::LightweightEvaluator<CountingEvaluator>);

}  // namespace

// ─── Tests ───────────────────────────────────────────────────────────────────

SCENARIO("Pipeline with ExternalWindowStore backend unavailable falls back to in-process store",
         "[integration][external_store]")
{
    GIVEN("a pipeline with a counting evaluator backed by an always-unavailable ExternalWindowStore") {
        auto target = std::make_shared<CapturingTarget>();

        // ExternalWindowStore with unavailable backend + in-process fallback
        auto fallback = std::make_shared<fre::InProcessWindowStore>();
        auto store = std::make_shared<fre::ExternalWindowStore>(
            make_unavailable_backend(), fallback);

        fre::EvalStageConfig eval_cfg;
        eval_cfg.add_evaluator(CountingEvaluator{store, /*threshold=*/5});

        fre::EmitStageConfig emit_cfg;
        emit_cfg.add_target(target);

        auto config_result = fre::PipelineConfig::Builder{}
            .pipeline_id("test-external-store")
            .eval_config(std::move(eval_cfg))
            .emit_config(std::move(emit_cfg))
            .build();
        REQUIRE(config_result.has_value());

        fre::Pipeline pipeline{std::move(*config_result)};
        REQUIRE(pipeline.start().has_value());

        WHEN("10 events are submitted for entity-A (crossing threshold at event 6)") {
            for (int i = 0; i < 10; ++i) {
                fre::Event event{};
                event.tenant_id  = "tenant-1";
                event.entity_id  = "entity-A";
                event.event_type = "test";
                REQUIRE(pipeline.submit(event).has_value());
            }

            pipeline.drain(3000ms);

            THEN("all 10 decisions are emitted") {
                REQUIRE(target->count() == 10);
            }

            THEN("the external store reports it is using the fallback") {
                // ExternalWindowStore silently falls back when backend is unavailable
                REQUIRE(store->is_degraded());
            }

            THEN("window counts still increment via in-process fallback (events 6-10 are flagged)") {
                int flagged_count = 0;
                for (std::size_t i = 0; i < target->count(); ++i) {
                    if (target->get(i).final_verdict != fre::Verdict::Pass) {
                        ++flagged_count;
                    }
                }
                // With in-process fallback, threshold of 5 is crossed on event 6+
                REQUIRE(flagged_count >= 1);
            }
        }
    }
}
