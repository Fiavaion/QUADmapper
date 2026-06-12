# Output library - Multi-window management and perform mode

add_library(quadmapper_output STATIC
    OutputManager.hpp
    OutputManager.cpp
    OutputWindow.hpp
    OutputWindow.cpp
    PerformMode.hpp
    PerformMode.cpp
)

target_include_directories(quadmapper_output
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_output
    PUBLIC
        quadmapper_core
        quadmapper_renderer
    PRIVATE
        glfw
)

set_target_properties(quadmapper_output PROPERTIES
    FOLDER "Libraries"
)
