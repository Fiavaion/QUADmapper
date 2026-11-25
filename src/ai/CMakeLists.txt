# AI pre-show integration boundary (ADR-035 / QM-056).
# Owns the optional Python sidecar lifecycle + loopback HTTP client. The Python
# sidecar itself (tools/ai-sidecar/) is NOT part of this build — it ships as an
# opt-in component and is never linked into QUADmapper.exe.

add_library(quadmapper_ai STATIC
    SidecarClient.hpp
)
# The engine link is WinHTTP and a child process in a Job Object. macOS has a stand-in
# that reports "not installed" until the link is ported (ADR-062).
if(WIN32)
    target_sources(quadmapper_ai PRIVATE SidecarClient_win.cpp)
else()
    target_sources(quadmapper_ai PRIVATE SidecarClient_mac.cpp)
endif()

target_include_directories(quadmapper_ai
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_ai
    PUBLIC
        quadmapper_core
    PRIVATE
        nlohmann_json::nlohmann_json
)
if(WIN32)
    target_link_libraries(quadmapper_ai PRIVATE winhttp)  # Windows system lib (HTTP client); no vcpkg dependency
endif()

set_target_properties(quadmapper_ai PROPERTIES
    FOLDER "Libraries"
)
