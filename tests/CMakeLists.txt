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
