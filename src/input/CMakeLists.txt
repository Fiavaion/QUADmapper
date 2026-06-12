# Input library - Keyboard shortcuts and mouse handling

add_library(quadmapper_input STATIC
    InputManager.hpp
    InputManager.cpp
    KeyboardShortcuts.hpp
    KeyboardShortcuts.cpp
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
        glfw
        nlohmann_json::nlohmann_json
        imgui
)

set_target_properties(quadmapper_input PROPERTIES
    FOLDER "Libraries"
)
