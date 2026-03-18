# ─── CPM.cmake bootstrap ─────────────────────────────────────────────────────
set(CPM_DOWNLOAD_VERSION 0.40.2)
set(CPM_DOWNLOAD_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")

if(NOT EXISTS "${CPM_DOWNLOAD_LOCATION}")
    message(STATUS "Downloading CPM.cmake v${CPM_DOWNLOAD_VERSION}...")
    file(DOWNLOAD
        "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake"
        "${CPM_DOWNLOAD_LOCATION}"
        EXPECTED_HASH SHA256=c8cdc32c03816538ce22781ed72964dc864b2a34a310d3b7104812a5ca2d835d
    )
endif()

include("${CPM_DOWNLOAD_LOCATION}")

# ─── Standalone Asio ─────────────────────────────────────────────────────────
CPMAddPackage(
    NAME asio
    VERSION 1.30.2
    URL "https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz"
    DOWNLOAD_ONLY YES
)

if(asio_ADDED)
    add_library(asio INTERFACE)
    add_library(asio::asio ALIAS asio)
    target_include_directories(asio INTERFACE "${asio_SOURCE_DIR}/asio/include")
    target_compile_definitions(asio INTERFACE
        ASIO_STANDALONE
        ASIO_NO_DEPRECATED
        ASIO_HAS_CO_AWAIT
    )
endif()

# ─── quill (structured logging) ──────────────────────────────────────────────
CPMAddPackage(
    NAME quill
    VERSION 4.5.0
    URL "https://github.com/odygrd/quill/archive/refs/tags/v4.5.0.tar.gz"
)

# ─── Catch2 (testing) ────────────────────────────────────────────────────────
if(FRE_BUILD_TESTS)
    CPMAddPackage(
        NAME Catch2
        VERSION 3.7.1
        URL "https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz"

    )

    if(Catch2_ADDED)
        list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
    endif()
endif()

# ─── nlohmann/json (REST API / debug output) ─────────────────────────────────
CPMAddPackage(
    NAME nlohmann_json
    VERSION 3.11.3
    URL "https://github.com/nlohmann/json/releases/download/v3.11.3/include.zip"

    DOWNLOAD_ONLY YES
)

if(nlohmann_json_ADDED)
    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json INTERFACE "${nlohmann_json_SOURCE_DIR}/include")
endif()

# ─── FlatBuffers (service harness binary serialization) ──────────────────────
if(FRE_BUILD_SERVICE)
    CPMAddPackage(
        NAME flatbuffers
        VERSION 24.3.25
        URL "https://github.com/google/flatbuffers/archive/refs/tags/v24.3.25.tar.gz"

        OPTIONS
            "FLATBUFFERS_BUILD_TESTS OFF"
            "FLATBUFFERS_BUILD_FLATLIB OFF"
            "FLATBUFFERS_BUILD_FLATHASH OFF"
    )
endif()

# ─── ONNX Runtime (optional, ML inference stage) ─────────────────────────────
if(FRE_ENABLE_ONNX)
    find_package(onnxruntime 1.19 REQUIRED)
endif()

# ─── DuckDB (optional, analytical external state backend) ─────────────────────
#
# Uses the DuckDB amalgamation (single .cpp + .h) to avoid the full DuckDB
# CMake build which has many sub-projects.  The amalgamation is MIT-licensed.
#
# Note: The amalgamation build is large (~5 min cold, ~200 MB object file).
#       CI should cache ${CMAKE_SOURCE_DIR}/_deps (or CPM_SOURCE_CACHE).
if(FRE_ENABLE_DUCKDB)
    CPMAddPackage(
        NAME duckdb
        VERSION 1.1.3
        URL "https://github.com/duckdb/duckdb/releases/download/v1.1.3/libduckdb-src.zip"
        DOWNLOAD_ONLY YES
    )

    if(duckdb_ADDED)
        add_library(duckdb_amalgamation STATIC
            "${duckdb_SOURCE_DIR}/duckdb.cpp"
        )
        add_library(duckdb::duckdb ALIAS duckdb_amalgamation)

        target_include_directories(duckdb_amalgamation
            PUBLIC "${duckdb_SOURCE_DIR}"
        )

        # Suppress warnings inside the amalgamation (not our code)
        target_compile_options(duckdb_amalgamation PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-w>
        )

        find_package(Threads REQUIRED)
        target_link_libraries(duckdb_amalgamation
            PUBLIC Threads::Threads ${CMAKE_DL_LIBS}
        )
    endif()
endif()
