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

fre_add_test(
    TARGET fre_unit_policy_leaf_nodes
    SOURCES test_policy_leaf_nodes.cpp
)

fre_add_test(
    TARGET fre_unit_decision_type_registry
    SOURCES test_decision_type_registry.cpp
)

fre_add_test(
    TARGET fre_unit_policy_multi_decision
    SOURCES test_policy_multi_decision.cpp
)

if(FRE_BUILD_SERVICE)
    add_executable(fre_unit_fleet_router test_fleet_router.cpp)
    target_link_libraries(fre_unit_fleet_router
        PRIVATE fre::pipeline fre::service Catch2::Catch2WithMain)
    target_compile_options(fre_unit_fleet_router PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    catch_discover_tests(fre_unit_fleet_router
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(FRE_ENABLE_DUCKDB)
    add_executable(fre_unit_duckdb_window_store test_duckdb_window_store.cpp)
    target_link_libraries(fre_unit_duckdb_window_store
        PRIVATE fre::pipeline Catch2::Catch2WithMain)
    target_compile_options(fre_unit_duckdb_window_store PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    catch_discover_tests(fre_unit_duckdb_window_store
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(fre_unit_write_back_window_store test_write_back_window_store.cpp)
    target_link_libraries(fre_unit_write_back_window_store
        PRIVATE fre::pipeline Catch2::Catch2WithMain)
    target_compile_options(fre_unit_write_back_window_store PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    catch_discover_tests(fre_unit_write_back_window_store
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(fre_unit_realistic_latency_benchmark test_realistic_latency_benchmark.cpp)
    target_link_libraries(fre_unit_realistic_latency_benchmark
        PRIVATE fre::pipeline Catch2::Catch2WithMain)
    target_compile_options(fre_unit_realistic_latency_benchmark PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    catch_discover_tests(fre_unit_realistic_latency_benchmark
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        EXTRA_ARGS --benchmark-samples 3
        PROPERTIES TIMEOUT 120)
endif()
