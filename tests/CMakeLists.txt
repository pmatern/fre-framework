include(CTest)
include(Catch)

# ─── Helper: fre_add_test ─────────────────────────────────────────────────────
# Usage: fre_add_test(TARGET my_test SOURCES src1.cpp src2.cpp)
function(fre_add_test)
    cmake_parse_arguments(ARG "" "TARGET;TIMEOUT" "SOURCES;EXTRA_ARGS" ${ARGN})

    add_executable(${ARG_TARGET} ${ARG_SOURCES})

    target_link_libraries(${ARG_TARGET}
        PRIVATE
            fre::pipeline
            Catch2::Catch2WithMain
    )

    target_compile_options(${ARG_TARGET} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )

    set(_discover_args
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(ARG_EXTRA_ARGS)
        list(APPEND _discover_args EXTRA_ARGS ${ARG_EXTRA_ARGS})
    endif()
    if(ARG_TIMEOUT)
        list(APPEND _discover_args PROPERTIES TIMEOUT ${ARG_TIMEOUT})
    endif()

    catch_discover_tests(${ARG_TARGET} ${_discover_args})
endfunction()

# ─── Subdirectories ──────────────────────────────────────────────────────────
add_subdirectory(contract)
add_subdirectory(unit)
add_subdirectory(integration)
