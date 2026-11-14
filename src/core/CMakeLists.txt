# Core library - Foundation utilities used by all components

add_library(quadmapper_core STATIC
    Types.hpp
    Version.hpp
    Result.hpp
    UUID.hpp
    UUID.cpp
    Logger.hpp
    Logger.cpp
    Timer.hpp
    Timer.cpp
    FileSystem.hpp
    FileSystem.cpp
    CommandHistory.hpp
    CommandHistory.cpp
    PerformanceMonitor.hpp
    PerformanceMonitor.cpp
    ErrorNotification.hpp
    ErrorNotification.cpp
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

set_target_properties(quadmapper_core PROPERTIES
    FOLDER "Libraries"
)
