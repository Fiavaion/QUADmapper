# Scene library - Quads, groups, and mesh subdivision

add_library(quadmapper_scene STATIC
    Scene.hpp
    Scene.cpp
    Quad.hpp
    Quad.cpp
    QuadGeometry.hpp
    QuadGeometry.cpp
    QuadGroup.hpp
    QuadGroup.cpp
    QuadManager.hpp
    QuadManager.cpp
    MeshSubdivision.hpp
    MeshSubdivision.cpp
    Commands.hpp
    Commands.cpp
    ProjectorCamera.hpp
    ProjectorCamera.cpp
    ProjectorCalibration.hpp
    ProjectorCalibration.cpp
    CalibrationSession.hpp
    CalibrationSession.cpp
    RayPick.hpp
    RayPick.cpp
    SmallLinAlg.hpp
    MeshObject.hpp
    ObjLoader.hpp
    ObjLoader.cpp
    MediaReferences.hpp
    Slice.hpp
    Slice.cpp
)

target_include_directories(quadmapper_scene
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_scene
    PUBLIC
        quadmapper_core
)

set_target_properties(quadmapper_scene PROPERTIES
    FOLDER "Libraries"
)
