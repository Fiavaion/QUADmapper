# QUADmapper - Professional Quad-Based Projection Mapping Application
# Root CMake Configuration

cmake_minimum_required(VERSION 3.20)

# Project definition
project(QUADmapper
    VERSION 1.0.0
    DESCRIPTION "Professional quad-based projection mapping application"
    LANGUAGES CXX C
)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for tooling (clangd, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type default
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# Options
option(QUADMAPPER_BUILD_TESTS "Build test suite" ON)
option(QUADMAPPER_ENABLE_ASAN "Enable AddressSanitizer (Debug only)" OFF)
option(QUADMAPPER_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
    if(QUADMAPPER_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(QUADMAPPER_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# vcpkg dependencies (found via toolchain)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# FFmpeg - using custom find module or vcpkg
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET
        libavcodec
        libavformat
        libavdevice
        libavutil
        libswscale
        libswresample
    )
endif()

# External dependencies (in external/)
add_subdirectory(external/glad)
add_subdirectory(external/imgui)
add_subdirectory(external/stb)

# Source libraries
add_subdirectory(src/core)
add_subdirectory(src/project)
add_subdirectory(src/scene)
add_subdirectory(src/renderer)
add_subdirectory(src/media)
add_subdirectory(src/video)
add_subdirectory(src/audio)
add_subdirectory(src/output)
add_subdirectory(src/source)
add_subdirectory(src/ui)
add_subdirectory(src/input)
add_subdirectory(src/shaders)

# Main application
add_subdirectory(src/app)

# Tests
if(QUADMAPPER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation
include(GNUInstallDirs)
install(TARGETS QUADmapper
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY resources/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/quadmapper
)

# Print configuration summary
message(STATUS "")
message(STATUS "QUADmapper Configuration Summary")
message(STATUS "================================")
message(STATUS "Version:          ${PROJECT_VERSION}")
message(STATUS "Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "Build tests:      ${QUADMAPPER_BUILD_TESTS}")
message(STATUS "")
