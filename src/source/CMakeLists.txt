# Source library - Unified texture source system

add_library(quadmapper_source STATIC
    # Core infrastructure
    FrameData.hpp
    TextureSource.hpp
    SourceManager.hpp
    SourceManager.cpp

    # Static image source
    StaticImageSource.hpp
    StaticImageSource.cpp

    # Video file source
    VideoFileSource.hpp
    VideoFileSource.cpp

    # Procedural sources
    procedural/ProceduralSource.hpp
    procedural/ProceduralSource.cpp
    procedural/ProceduralFactory.hpp
    procedural/ProceduralFactory.cpp
    procedural/SolidColorSource.hpp
    procedural/SolidColorSource.cpp
    procedural/GradientSource.hpp
    procedural/GradientSource.cpp
    procedural/CheckerboardSource.hpp
    procedural/CheckerboardSource.cpp
    procedural/TestPatternSource.hpp
    procedural/TestPatternSource.cpp
    procedural/NoiseSource.hpp
    procedural/NoiseSource.cpp
    procedural/PlasmaSource.hpp
    procedural/PlasmaSource.cpp
)

target_include_directories(quadmapper_source
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_source
    PUBLIC
        quadmapper_core
        quadmapper_renderer
        quadmapper_media
        quadmapper_video
        glad
        glm::glm
)

set_target_properties(quadmapper_source PROPERTIES
    FOLDER "Libraries"
)
