# Unit tests — one target per component

fre_add_test(
    TARGET fre_unit_core_types
    SOURCES test_core_types.cpp
)

fre_add_test(
    TARGET fre_unit_tenant_router
    SOURCES test_tenant_router.cpp
)

fre_add_test(
    TARGET fre_unit_pipeline_config
    SOURCES test_pipeline_config.cpp
)

fre_add_test(
    TARGET fre_unit_eval_stage_composition
    SOURCES test_eval_stage_composition.cpp
)

fre_add_test(
    TARGET fre_unit_window_store
    SOURCES test_window_store.cpp
)

fre_add_test(
    TARGET fre_unit_allow_deny_evaluator
    SOURCES test_allow_deny_evaluator.cpp
)

fre_add_test(
    TARGET fre_unit_latency_benchmark
    SOURCES test_latency_benchmark.cpp
    EXTRA_ARGS --benchmark-samples 3
    TIMEOUT 120
)

fre_add_test(
    TARGET fre_unit_sync_submit
    SOURCES test_sync_submit.cpp
    TIMEOUT 60
)
