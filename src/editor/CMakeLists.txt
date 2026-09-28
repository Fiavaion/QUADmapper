# Editor library (ADR-051 §1): the editor's state and logic, below ui and app.
# No panel and no app code: the layering_check ctest fails on any ui/ or app/ include
# here. ImGui comes in only through input/VertexKeyboard.hpp (the canvas keys, ADR-051 D3).

add_library(quadmapper_editor STATIC
    EditorState.hpp
    EditorContext.hpp
    ControlBindings.hpp
    GridUndo.hpp
    MediaAssign.hpp
    MeshEditKeys.hpp
    MeshLoads.hpp
    MissingMedia.hpp
    OutputPersistence.hpp
    Preferences.hpp
    Preferences.cpp
    ProjectMapping.hpp
    ProjectMapping.cpp
    ProjectReplace.hpp
    QuadSelection.hpp
    ReplaceMedia.hpp
    SliceEditing.hpp
    SourceAudio.hpp
    StatusLine.hpp
    TextureResolve.hpp
    UndoRouting.hpp
    UnsavedIndicator.hpp
)

target_include_directories(quadmapper_editor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_editor
    PUBLIC
        quadmapper_core
        quadmapper_scene
        quadmapper_project
        quadmapper_renderer
        quadmapper_output
        quadmapper_source
        quadmapper_media
        quadmapper_video
        quadmapper_audio
        quadmapper_control
        quadmapper_input
    PRIVATE
        nlohmann_json::nlohmann_json
)

set_target_properties(quadmapper_editor PROPERTIES
    FOLDER "Libraries"
)
