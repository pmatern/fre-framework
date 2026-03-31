# Integration tests — full pipeline end-to-end

fre_add_test(
    TARGET fre_integration_minimal_pipeline
    SOURCES test_minimal_pipeline.cpp
)

fre_add_test(
    TARGET fre_integration_custom_evaluator
    SOURCES test_custom_evaluator.cpp
)

fre_add_test(
    TARGET fre_integration_windowed
    SOURCES test_windowed_isolation.cpp
)

fre_add_test(
    TARGET fre_integration_ml_inference
    SOURCES test_ml_inference_stage.cpp
)

fre_add_test(
    TARGET fre_integration_inference_timeout
    SOURCES test_inference_timeout.cpp
)

fre_add_test(
    TARGET fre_integration_policy_evaluation
    SOURCES test_policy_evaluation.cpp
)

fre_add_test(
    TARGET fre_integration_policy_config_validation
    SOURCES test_policy_config_validation.cpp
)

fre_add_test(
    TARGET fre_integration_external_store_fallback
    SOURCES test_external_store_fallback.cpp
)

fre_add_test(
    TARGET fre_integration_load_p99
    SOURCES test_load_p99.cpp
)

fre_add_test(
    TARGET fre_integration_sync_submit
    SOURCES test_sync_submit_integration.cpp
    TIMEOUT 120
)

fre_add_test(
    TARGET fre_integration_policy_leaf_nodes
    SOURCES test_policy_leaf_node_integration.cpp
    TIMEOUT 60
)

if(FRE_BUILD_SERVICE)
    add_executable(fre_integration_service_harness test_service_harness.cpp)
    target_link_libraries(fre_integration_service_harness
        PRIVATE fre::pipeline fre::service Catch2::Catch2WithMain)
    target_compile_options(fre_integration_service_harness PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    catch_discover_tests(fre_integration_service_harness
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(fre_integration_fleet_routing test_fleet_routing.cpp)
    target_link_libraries(fre_integration_fleet_routing
        PRIVATE fre::pipeline fre::service Catch2::Catch2WithMain)
    target_compile_options(fre_integration_fleet_routing PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    catch_discover_tests(fre_integration_fleet_routing
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        PROPERTIES TIMEOUT 60)
endif()

if(FRE_ENABLE_DUCKDB)
    add_executable(fre_integration_write_back_pipeline test_write_back_pipeline.cpp)
    target_link_libraries(fre_integration_write_back_pipeline
        PRIVATE fre::pipeline Catch2::Catch2WithMain)
    target_compile_options(fre_integration_write_back_pipeline PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    catch_discover_tests(fre_integration_write_back_pipeline
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        PROPERTIES TIMEOUT 120)

    add_executable(fre_integration_duckdb_backend test_duckdb_backend.cpp)
    target_link_libraries(fre_integration_duckdb_backend
        PRIVATE fre::pipeline Catch2::Catch2WithMain)
    target_compile_options(fre_integration_duckdb_backend PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    catch_discover_tests(fre_integration_duckdb_backend
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        PROPERTIES TIMEOUT 120)

    add_executable(fre_integration_realistic_load_p99 test_realistic_load_p99.cpp)
    target_link_libraries(fre_integration_realistic_load_p99
        PRIVATE fre::pipeline Catch2::Catch2WithMain)
    target_compile_options(fre_integration_realistic_load_p99 PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    catch_discover_tests(fre_integration_realistic_load_p99
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        PROPERTIES TIMEOUT 180)
endif()
