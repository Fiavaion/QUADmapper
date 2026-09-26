# Input library - vertex keyboard maths (VertexKeyboard.hpp, header-only)

add_library(quadmapper_input INTERFACE)

target_include_directories(quadmapper_input
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_input
    INTERFACE
        quadmapper_core
        imgui
)
