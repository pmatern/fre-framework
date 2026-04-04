#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/decision_type.hpp>
#include <fre/core/error.hpp>
#include <fre/core/verdict.hpp>
#include <fre/policy/rule_engine.hpp>
#include <fre/sharding/tenant_router.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fre {

// ─── FailureMode ─────────────────────────────────────────────────────────────

enum class FailureMode : uint8_t {
    /// Treat the stage as passed (verdict = Pass, skipped = true).
    FailOpen,
    /// Treat the stage as blocked (verdict = Block, skipped = true).
    FailClosed,
    /// Emit a degraded decision (verdict = Pass, DegradedReason bit set).
    EmitDegraded,
};

// ─── Type-erased evaluator and emission target handles ───────────────────────

using EvaluatorFn =
    std::function<std::expected<EvaluatorResult, EvaluatorError>(const Event&)>;

using EmissionFn =
    std::function<std::expected<void, EmissionError>(Decision)>;

// ─── IngestStageConfig ───────────────────────────────────────────────────────

struct IngestStageConfig {
    /// Maximum wall-clock skew between event timestamp and receipt time.
    /// Events arriving more than this in the past are flagged as late.
    std::chrono::milliseconds skew_tolerance{std::chrono::seconds{30}};

    /// Timeout for the ingest stage itself.
    std::chrono::milliseconds timeout{std::chrono::milliseconds{5}};
};

// ─── EvalStageConfig ─────────────────────────────────────────────────────────

struct EvalStageConfig {
    std::chrono::milliseconds timeout{std::chrono::milliseconds{10}};
    FailureMode               failure_mode{FailureMode::FailOpen};
    CompositionRule           composition{CompositionRule::AnyBlock};
    std::vector<EvaluatorFn>  evaluators;

    /// Register a lightweight evaluator (satisfies LightweightEvaluator concept).
    template <LightweightEvaluator E>
    EvalStageConfig& add_evaluator(E&& e) & {
        evaluators.emplace_back(
            [ev = std::forward<E>(e)](const Event& event) mutable {
                return ev.evaluate(event);
            });
        return *this;
    }

    template <LightweightEvaluator E>
    EvalStageConfig&& add_evaluator(E&& e) && {
        evaluators.emplace_back(
            [ev = std::forward<E>(e)](const Event& event) mutable {
                return ev.evaluate(event);
            });
        return std::move(*this);
    }

    /// Register any callable with signature compatible with EvaluatorFn.
    EvalStageConfig& add_evaluator(EvaluatorFn fn) & {
        evaluators.push_back(std::move(fn));
        return *this;
    }

    EvalStageConfig&& add_evaluator(EvaluatorFn fn) && {
        evaluators.push_back(std::move(fn));
        return std::move(*this);
    }
};

// ─── InferenceStageConfig ─────────────────────────────────────────────────────

struct InferenceStageConfig {
    std::chrono::milliseconds timeout{std::chrono::milliseconds{200}};
    FailureMode               failure_mode{FailureMode::FailOpen};
    CompositionRule           composition{CompositionRule::WeightedScore};
    float                     score_threshold{0.5f};
    std::vector<EvaluatorFn>  evaluators;

    template <InferenceEvaluator E>
    InferenceStageConfig& add_evaluator(E&& e) & {
        evaluators.emplace_back(
            [ev = std::forward<E>(e)](const Event& event) mutable {
                return ev.evaluate(event);
            });
        return *this;
    }

    template <InferenceEvaluator E>
    InferenceStageConfig&& add_evaluator(E&& e) && {
        evaluators.emplace_back(
            [ev = std::forward<E>(e)](const Event& event) mutable {
                return ev.evaluate(event);
            });
        return std::move(*this);
    }

    /// Register any callable with signature compatible with EvaluatorFn.
    InferenceStageConfig& add_evaluator(EvaluatorFn fn) & {
        evaluators.push_back(std::move(fn));
        return *this;
    }

    InferenceStageConfig&& add_evaluator(EvaluatorFn fn) && {
        evaluators.push_back(std::move(fn));
        return std::move(*this);
    }
};

// ─── PolicyStageRule ─────────────────────────────────────────────────────────

/// A single rule entry in a PolicyStageConfig.
struct PolicyStageRule {
    policy::PolicyRule rule;
    int                priority{0};    ///< Lower value = evaluated first.
    Verdict            action_verdict{Verdict::Block};
    std::string        rule_id;

    /// When set, this rule participates in multi-decision evaluation:
    /// all matching rules are collected, combinability constraints are applied,
    /// and each surviving match emits an ActiveDecision of this type.
    /// nullopt = legacy first-match-wins behaviour (unchanged).
    std::optional<std::string> decision_type_id;
};

// ─── PolicyStageConfig ───────────────────────────────────────────────────────

struct PolicyStageConfig {
    std::chrono::milliseconds      timeout{std::chrono::milliseconds{20}};
    FailureMode                    failure_mode{FailureMode::FailClosed};
    std::vector<PolicyStageRule>   rules;

    /// Register a declarative policy rule.
    /// Rules are evaluated in ascending priority order; first match wins.
    PolicyStageConfig& add_rule(policy::PolicyRule rule,
                                int priority,
                                Verdict action_verdict,
                                std::string rule_id) & {
        rules.push_back(PolicyStageRule{
            std::move(rule), priority, action_verdict, std::move(rule_id)});
        std::sort(rules.begin(), rules.end(),
                  [](const PolicyStageRule& a, const PolicyStageRule& b) {
                      return a.priority < b.priority;
                  });
        return *this;
    }

    PolicyStageConfig&& add_rule(policy::PolicyRule rule,
                                 int priority,
                                 Verdict action_verdict,
                                 std::string rule_id) && {
        rules.push_back(PolicyStageRule{
            std::move(rule), priority, action_verdict, std::move(rule_id)});
        std::sort(rules.begin(), rules.end(),
                  [](const PolicyStageRule& lhs, const PolicyStageRule& rhs) {
                      return lhs.priority < rhs.priority;
                  });
        return std::move(*this);
    }

    /// Register a rule that participates in multi-decision evaluation.
    /// When a DecisionTypeRegistry is configured on the pipeline, all matching
    /// rules (not just the first) are collected, combinability constraints are
    /// applied, and each surviving match emits an ActiveDecision of the given type.
    PolicyStageConfig& add_rule(policy::PolicyRule rule,
                                int priority,
                                Verdict action_verdict,
                                std::string rule_id,
                                std::string decision_type_id) & {
        PolicyStageRule psr{std::move(rule), priority, action_verdict, std::move(rule_id)};
        psr.decision_type_id = std::move(decision_type_id);
        rules.push_back(std::move(psr));
        std::sort(rules.begin(), rules.end(),
                  [](const PolicyStageRule& lhs, const PolicyStageRule& rhs) {
                      return lhs.priority < rhs.priority;
                  });
        return *this;
    }

    PolicyStageConfig&& add_rule(policy::PolicyRule rule,
                                 int priority,
                                 Verdict action_verdict,
                                 std::string rule_id,
                                 std::string decision_type_id) && {
        PolicyStageRule psr{std::move(rule), priority, action_verdict, std::move(rule_id)};
        psr.decision_type_id = std::move(decision_type_id);
        rules.push_back(std::move(psr));
        std::sort(rules.begin(), rules.end(),
                  [](const PolicyStageRule& lhs, const PolicyStageRule& rhs) {
                      return lhs.priority < rhs.priority;
                  });
        return std::move(*this);
    }

    [[nodiscard]] std::string_view stage_id() const noexcept { return "policy"; }
};

// ─── EmitStageConfig ─────────────────────────────────────────────────────────

struct EmitStageConfig {
    std::chrono::milliseconds timeout{std::chrono::milliseconds{10}};
    FailureMode               failure_mode{FailureMode::EmitDegraded};
    uint32_t                  retry_limit{3};
    std::vector<EmissionFn>   targets;

    template <EmissionTarget E>
    EmitStageConfig& add_target(std::shared_ptr<E> t) & {
        targets.emplace_back(
            [target = std::move(t)](Decision d) {
                return target->emit(std::move(d));
            });
        return *this;
    }

    template <EmissionTarget E>
    EmitStageConfig&& add_target(std::shared_ptr<E> t) && {
        targets.emplace_back(
            [target = std::move(t)](Decision d) {
                return target->emit(std::move(d));
            });
        return std::move(*this);
    }

    template <EmissionTarget E>
    EmitStageConfig& add_target(E&& t) & {
        targets.emplace_back(
            [target = std::forward<E>(t)](Decision d) mutable {
                return target.emit(std::move(d));
            });
        return *this;
    }

    /// Convenience: add a built-in target that prints decision summaries to stdout.
    EmitStageConfig& add_stdout_target() & {
        targets.emplace_back(
            [](Decision d) -> std::expected<void, EmissionError> {
                std::printf("[emit] pipeline=%s event=%lu verdict=%d\n",
                    d.pipeline_id.c_str(),
                    static_cast<unsigned long>(d.event_id),
                    static_cast<int>(d.final_verdict));
                return {};
            });
        return *this;
    }

    EmitStageConfig&& add_stdout_target() && {
        add_stdout_target();
        return std::move(*this);
    }

    /// Convenience: add a no-op target that discards all decisions (useful in tests
    /// where a capturing harness is injected after build()).
    EmitStageConfig& add_noop_target() & {
        targets.emplace_back(
            [](Decision) -> std::expected<void, EmissionError> {
                return {};
            });
        return *this;
    }

    EmitStageConfig&& add_noop_target() && {
        add_noop_target();
        return std::move(*this);
    }
};

// ─── PipelineConfig ───────────────────────────────────────────────────────────

struct PipelineConfig {
    std::string                       pipeline_id;
    std::string                       pipeline_version{"1.0.0"};
    std::chrono::milliseconds         latency_budget{std::chrono::milliseconds{300}};
    ShardingConfig                    sharding;
    RateLimitConfig                   rate_limit;
    IngestStageConfig                 ingest_config;
    std::optional<EvalStageConfig>    eval_config;
    std::optional<InferenceStageConfig> inference_config;
    std::optional<PolicyStageConfig>  policy_config;
    EmitStageConfig                   emit_config;  // always required

    /// Optional registry of named decision types and their combinability rules.
    /// When present, policy rules with a decision_type_id participate in
    /// multi-decision evaluation (all-rules, then combinability filtering).
    /// When absent, all policy rules use legacy first-match-wins semantics.
    std::optional<DecisionTypeRegistry> decision_type_registry;

    // ─── Builder ─────────────────────────────────────────────────────────────
    // Defined after PipelineConfig is complete (below) because it stores a
    // PipelineConfig by value, which requires the complete type.
    class Builder;
};

// PipelineConfig is complete here — Builder can now store it by value.
class PipelineConfig::Builder {
public:
    Builder& pipeline_id(std::string id) {
        config_.pipeline_id = std::move(id);
        return *this;
    }

    Builder& pipeline_version(std::string v) {
        config_.pipeline_version = std::move(v);
        return *this;
    }

    Builder& latency_budget(std::chrono::milliseconds budget) {
        config_.latency_budget = budget;
        return *this;
    }

    Builder& ingest(IngestStageConfig cfg) {
        config_.ingest_config = std::move(cfg);
        return *this;
    }

    // Legacy names (kept for backwards compat)
    Builder& lightweight_eval(EvalStageConfig cfg) {
        config_.eval_config = std::move(cfg);
        return *this;
    }

    Builder& ml_inference(InferenceStageConfig cfg) {
        config_.inference_config = std::move(cfg);
        return *this;
    }

    Builder& policy_eval(PolicyStageConfig cfg) {
        config_.policy_config = std::move(cfg);
        return *this;
    }

    Builder& emit(EmitStageConfig cfg) {
        config_.emit_config = std::move(cfg);
        has_emit_           = true;
        return *this;
    }

    // Alias names used in tests / integration code
    Builder& eval_config(EvalStageConfig cfg) {
        return lightweight_eval(std::move(cfg));
    }

    Builder& inference_config(InferenceStageConfig cfg) {
        return ml_inference(std::move(cfg));
    }

    Builder& policy_config(PolicyStageConfig cfg) {
        return policy_eval(std::move(cfg));
    }

    Builder& decision_types(DecisionTypeRegistry registry) {
        config_.decision_type_registry = std::move(registry);
        return *this;
    }

    Builder& emit_config(EmitStageConfig cfg) {
        return emit(std::move(cfg));
    }

    Builder& sharding(ShardingConfig cfg);
    Builder& rate_limit(RateLimitConfig cfg);

    [[nodiscard]] std::expected<PipelineConfig, Error> build();

private:
    PipelineConfig config_;
    bool           has_emit_{false};

    [[nodiscard]] std::optional<ConfigError> validate() const;
};

}  // namespace fre
