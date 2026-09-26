# QUADmapper Test Suite
# Uses GoogleTest framework via FetchContent

include(FetchContent)

# Fetch GoogleTest
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)

# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

enable_testing()

# Helper function to create test executables
function(add_quadmapper_test TEST_NAME)
    add_executable(${TEST_NAME} ${ARGN})
    target_link_libraries(${TEST_NAME} PRIVATE
        GTest::gtest_main
        GTest::gmock
    )
    target_include_directories(${TEST_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/src
    )

    include(GoogleTest)
    # TD-07: the default 5s discovery timeout races a loaded machine and makes the
    # post-build discovery step (and thus `cmake --build`) return non-zero while tests
    # are actually green. A generous timeout removes the race without changing the
    # discovery mode, so `ctest --test-dir build/debug` keeps working as-is.
    gtest_discover_tests(${TEST_NAME} DISCOVERY_TIMEOUT 60)
endfunction()

# Unit tests by module
add_subdirectory(unit)

# Integration tests
add_subdirectory(integration)

# ---------------------------------------------------------------------------
# stress_smoke — QM-081 H3: fast CI gate for the perform-mode stress harness.
# Runs 5 cycles, 1 output, 20 frames/cycle in --hidden mode (no screen grab).
# PASS = exit code 0 (GL flat, no wedge, working-set within default 64 MB bound).
# The full 50-cycle release gate is a manual / /release-time check only.
# ---------------------------------------------------------------------------
add_test(
    NAME stress_smoke
    COMMAND $<TARGET_FILE:QUADmapper>
        --stress
        --cycles=5
        --outputs=1
        --frames-per-cycle=20
        --hidden
)
set_tests_properties(stress_smoke PROPERTIES
    TIMEOUT 60   # generous wall-clock limit; each cycle budget is 5 s × 5 = 25 s
)

# ---------------------------------------------------------------------------
# bench_smoke — the PRD §5 benchmark harness (--bench) runs every scenario briefly
# and writes a report it can read back (exit 0). Performance targets are NOT
# asserted here: machines vary. Video scenarios report "skipped" when the default
# media (D:/testing Videos) is absent. Full runs: see Bench.hpp.
# ---------------------------------------------------------------------------
add_test(
    NAME bench_smoke
    COMMAND $<TARGET_FILE:QUADmapper>
        --bench
        --bench-out=${CMAKE_CURRENT_BINARY_DIR}/bench_smoke.json
        --bench-seconds=0.5
        --bench-warmup=0.2
        --bench-quads=16
        --bench-videos=2
        --bench-scenes=3
        --bench-switch-every=3
        --bench-latency-samples=3
        --bench-drift-seconds=1
)
set_tests_properties(bench_smoke PROPERTIES
    TIMEOUT 120
)
