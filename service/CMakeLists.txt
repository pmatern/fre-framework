# ─── Service harness library (used by binary and integration tests) ───────────

add_library(fre_service_lib STATIC
    src/harness.cpp
    src/fleet_router.cpp
    ../src/service/decision_serializer.cpp
)

add_library(fre::service ALIAS fre_service_lib)

target_link_libraries(fre_service_lib
    PUBLIC
        fre::pipeline
        nlohmann_json::nlohmann_json
)

target_include_directories(fre_service_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(fre_service_lib PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# ─── fre-service binary ───────────────────────────────────────────────────────

add_executable(fre-service
    src/main.cpp
)

target_link_libraries(fre-service
    PRIVATE
        fre::service
)

target_compile_options(fre-service PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
