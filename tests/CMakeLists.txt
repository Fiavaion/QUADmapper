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

# layering_check (ADR-051 §1): src/editor includes neither ui/ nor app/, src/ui no app/.
add_test(
    NAME layering_check
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/src
        -P ${CMAKE_CURRENT_SOURCE_DIR}/LayeringCheck.cmake
)

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
# asserted here: machines vary. Video scenarios report "skipped" when the test
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
        "--bench-media=D:/testing Videos/greenCrystals_15Sec.mp4"
        "--bench-hap-media=D:/testing Videos/Hap/5sec_hap.mov"
)
set_tests_properties(bench_smoke PROPERTIES
    TIMEOUT 120
)
# stress_audio_smoke — ADR-050 §2: a video with sound, re-created every cycle, so its
# audio attachment (decoder thread, mixer input) is built and freed ten times under the
# working-set bound, on the silent null audio device. Needs the local test media.
set(QM_AUDIO_STRESS_MEDIA "D:/testing Videos/keying woman.mp4")
if(EXISTS "${QM_AUDIO_STRESS_MEDIA}")
    add_test(
        NAME stress_audio_smoke
        COMMAND $<TARGET_FILE:QUADmapper>
            --stress
            --cycles=10
            --outputs=1
            --frames-per-cycle=30
            --hidden
            --source=video
            --audio-churn
            "--media=${QM_AUDIO_STRESS_MEDIA}"
    )
    set_tests_properties(stress_audio_smoke PROPERTIES TIMEOUT 120)
    # stress_slow_open_smoke — TD-35: video files open on the opener thread, so a slow
    # open must not count as a wedge. Every open here waits 6 s, longer than the 5 s
    # cycle budget; the re-created sources' cycles must still pass.
    add_test(
        NAME stress_slow_open_smoke
        COMMAND $<TARGET_FILE:QUADmapper>
            --stress
            --cycles=2
            --outputs=1
            --frames-per-cycle=30
            --hidden
            --source=video
            --audio-churn
            --open-delay-ms=6000
            "--media=${QM_AUDIO_STRESS_MEDIA}"
    )
    set_tests_properties(stress_slow_open_smoke PROPERTIES TIMEOUT 120)
endif()
# stress_video_smoke — ADR-056: a 10-bit HEVC clip played in hardware (zero-copy where the
# GL driver can share D3D11 textures) through perform cycles; after the source is
# removed no interop object, GPU frame, staging texture or copy query may remain.
# Registered only when the local test media exists (not in CI).
set(QM_VIDEO_STRESS_MEDIA "D:/testing Videos/qa_video/hevc10_1080p.mp4")
if(EXISTS "${QM_VIDEO_STRESS_MEDIA}")
    add_test(
        NAME stress_video_smoke
        COMMAND $<TARGET_FILE:QUADmapper>
            --stress
            --cycles=5
            --outputs=1
            --frames-per-cycle=30
            --hidden
            --source=video
            "--media=${QM_VIDEO_STRESS_MEDIA}"
    )
    set_tests_properties(stress_video_smoke PROPERTIES TIMEOUT 120)
endif()
