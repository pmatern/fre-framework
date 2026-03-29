cmake_minimum_required(VERSION 3.28)

project(fre
    VERSION 1.1.0
    DESCRIPTION "Near Real-Time Detection Pipeline Framework"
    HOMEPAGE_URL "https://github.com/your-org/fre-framework"
    LANGUAGES CXX
)

# ─── C++ standard ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Build type defaults ─────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

# ─── Options ─────────────────────────────────────────────────────────────────
option(FRE_BUILD_TESTS    "Build tests"              ON)
option(FRE_BUILD_EXAMPLES "Build examples"           ON)
option(FRE_BUILD_SERVICE  "Build service harness"    ON)
option(FRE_ENABLE_ONNX    "Enable ML inference stage (requires ONNX Runtime)" OFF)
option(FRE_ENABLE_DUCKDB  "Enable DuckDB analytical state backend" OFF)
option(FRE_CLANG_TIDY     "Enable clang-tidy during build (warnings-as-errors)" OFF)

# ─── Version header ──────────────────────────────────────────────────────────
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/fre/version.hpp"
    @ONLY
)

# ─── Dependencies ────────────────────────────────────────────────────────────
include(cmake/dependencies.cmake)

# ─── Core library ────────────────────────────────────────────────────────────
add_library(fre_pipeline)
add_library(fre::pipeline ALIAS fre_pipeline)

target_sources(fre_pipeline PRIVATE
    src/pipeline/error.cpp
    src/pipeline/logging.cpp
    src/pipeline/pipeline.cpp
    src/pipeline/pipeline_config.cpp
    src/stage/ingest_stage.cpp
    src/stage/eval_stage.cpp
    src/stage/emit_stage.cpp
    src/stage/inference_stage.cpp
    src/evaluator/allow_deny_evaluator.cpp
    src/evaluator/plugin_loader.cpp
    src/state/window_store.cpp
    src/state/external_store.cpp
    src/policy/rule_engine.cpp
    src/stage/policy_stage.cpp
    src/testing/pipeline_harness.cpp
    src/sharding/tenant_router.cpp
)

target_include_directories(fre_pipeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(fre_pipeline
    PUBLIC
        $<BUILD_INTERFACE:asio::asio>
        $<BUILD_INTERFACE:quill::quill>
    PRIVATE
        # implementation-only linkage
)

target_compile_options(fre_pipeline PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# ─── clang-tidy (FR-018) ─────────────────────────────────────────────────────
if(FRE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 clang-tidy-17)
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY
            "${CLANG_TIDY_EXE}"
            "--warnings-as-errors=*"
            "--extra-arg=-std=c++23"
        )
        message(STATUS "clang-tidy enabled: ${CLANG_TIDY_EXE}")
    else()
        message(WARNING "FRE_CLANG_TIDY=ON but clang-tidy executable not found; skipping")
    endif()
endif()

if(FRE_ENABLE_ONNX)
    target_sources(fre_pipeline PRIVATE
        src/evaluator/onnx_inference_evaluator.cpp
    )
    target_link_libraries(fre_pipeline PUBLIC onnxruntime)
    target_compile_definitions(fre_pipeline PUBLIC FRE_ENABLE_ONNX)
endif()

if(FRE_ENABLE_DUCKDB)
    target_sources(fre_pipeline PRIVATE
        src/state/duckdb_window_store.cpp
        src/state/write_back_window_store.cpp
    )
    target_link_libraries(fre_pipeline PUBLIC $<BUILD_INTERFACE:duckdb::duckdb>)
    target_compile_definitions(fre_pipeline PUBLIC FRE_ENABLE_DUCKDB)
endif()

# ─── Service harness ─────────────────────────────────────────────────────────
if(FRE_BUILD_SERVICE)
    add_subdirectory(service)
endif()

# ─── Tests ───────────────────────────────────────────────────────────────────
if(FRE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Examples ────────────────────────────────────────────────────────────────
if(FRE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ─── Coverage report target (FR-017) ─────────────────────────────────────────
# Usage: cmake --preset coverage && cmake --build --preset coverage && cmake --build --preset coverage --target coverage
find_program(LCOV_EXE       NAMES lcov)
find_program(GENHTML_EXE    NAMES genhtml)
find_program(LLVM_COV_EXE   NAMES llvm-cov)
find_program(LLVM_PROFDATA  NAMES llvm-profdata)

if(LCOV_EXE AND GENHTML_EXE)
    add_custom_target(coverage
        COMMAND ctest --preset coverage --output-on-failure
        COMMAND ${LCOV_EXE}
            --capture
            --directory "${CMAKE_BINARY_DIR}"
            --output-file "${CMAKE_BINARY_DIR}/coverage.info"
            --exclude "*/build/_deps/*"
            --exclude "*/tests/*"
            --exclude "/usr/*"
        COMMAND ${GENHTML_EXE}
            "${CMAKE_BINARY_DIR}/coverage.info"
            --output-directory "${CMAKE_BINARY_DIR}/coverage-html"
        COMMAND ${LCOV_EXE}
            --summary "${CMAKE_BINARY_DIR}/coverage.info"
        COMMENT "Generating coverage report in ${CMAKE_BINARY_DIR}/coverage-html/index.html"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
elseif(LLVM_COV_EXE AND LLVM_PROFDATA)
    add_custom_target(coverage
        COMMAND ctest --preset coverage --output-on-failure
        COMMENT "Run 'scripts/ci/check-coverage.sh' after build to evaluate threshold"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
else()
    message(STATUS "Coverage target unavailable: install lcov+genhtml or llvm-cov+llvm-profdata")
endif()

# ─── Install ─────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS fre_pipeline
    EXPORT FreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/fre
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/fre/version.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fre
)

install(EXPORT FreTargets
    FILE FreTargets.cmake
    NAMESPACE fre::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fre
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FreConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/FreConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fre
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/FreConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/FreConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/FreConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fre
)
