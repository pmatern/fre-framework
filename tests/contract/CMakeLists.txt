# Contract / concept-satisfaction tests

fre_add_test(
    TARGET fre_contract_pipeline
    SOURCES test_pipeline_contract.cpp
)

fre_add_test(
    TARGET fre_contract_evaluator_concepts
    SOURCES test_evaluator_concepts.cpp
)

fre_add_test(
    TARGET fre_contract_custom_evaluator
    SOURCES test_custom_evaluator.cpp
)

fre_add_test(
    TARGET fre_contract_threshold
    SOURCES test_threshold_contract.cpp
)

fre_add_test(
    TARGET fre_contract_inference_concept
    SOURCES test_inference_concept.cpp
)

fre_add_test(
    TARGET fre_contract_policy_concept
    SOURCES test_policy_concept.cpp
)

fre_add_test(
    TARGET fre_contract_policy_rule
    SOURCES test_policy_rule_contract.cpp
)
