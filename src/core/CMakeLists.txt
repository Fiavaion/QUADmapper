# Core library - Foundation utilities used by all components

add_library(quadmapper_core STATIC
    Types.hpp
    Version.hpp
    Result.hpp
    UUID.hpp
    UUID.cpp
    Logger.hpp
    Logger.cpp
    FileSystem.hpp
    FileSystem.cpp
    CommandHistory.hpp
    CommandHistory.cpp
    GlObjectStats.hpp
    GlObjectStats.cpp
)

target_include_directories(quadmapper_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_core
    PUBLIC
        glm::glm
)

# Version.hpp: the version is set once, by project(VERSION) in the root CMakeLists.txt.
target_compile_definitions(quadmapper_core
    PUBLIC
        QUADMAPPER_VERSION="${PROJECT_VERSION}"
)

set_target_properties(quadmapper_core PROPERTIES
    FOLDER "Libraries"
)
