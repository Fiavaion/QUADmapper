# Project library - Save/load and serialization

add_library(quadmapper_project STATIC
    Project.hpp
    Project.cpp
    ProjectSerializer.hpp
    ProjectSerializer.cpp
    ProjectSettings.hpp
    ProjectSettings.cpp
)

target_include_directories(quadmapper_project
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_project
    PUBLIC
        quadmapper_core
        nlohmann_json::nlohmann_json
)

set_target_properties(quadmapper_project PROPERTIES
    FOLDER "Libraries"
)
