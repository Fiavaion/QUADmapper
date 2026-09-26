# Input library - vertex keyboard maths and mouse handling

add_library(quadmapper_input STATIC
    VertexKeyboard.hpp
    MouseHandler.hpp
    MouseHandler.cpp
)

target_include_directories(quadmapper_input
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_input
    PUBLIC
        quadmapper_core
    PRIVATE
        imgui
)

set_target_properties(quadmapper_input PROPERTIES
    FOLDER "Libraries"
)
