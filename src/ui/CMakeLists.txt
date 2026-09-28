# UI library - Dear ImGui interface

add_library(quadmapper_ui STATIC
    Theme.hpp
    Theme.cpp
    AsyncFileDialog.hpp
    AsyncFileDialog.cpp
    EditorPanels.hpp
    panels/Panel.hpp
    panels/InspectorPanel.hpp
    panels/InspectorPanel.cpp
    panels/ViewportEditor.hpp
    panels/ViewportEditor.cpp
    panels/OutputPanel.hpp
    panels/OutputPanel.cpp
    panels/MainMenuBar.hpp
    panels/MainMenuBar.cpp
    panels/QuadListPanel.hpp
    panels/QuadListPanel.cpp
    panels/ScenePanel.hpp
    panels/ScenePanel.cpp
    panels/SliceEditor.hpp
    panels/SliceEditor.cpp
    panels/OutputPreviewPanel.hpp
    panels/OutputPreviewPanel.cpp
    panels/VideoPlayerPanel.hpp
    panels/VideoPlayerPanel.cpp
    panels/MediaBinPanel.hpp
    panels/MediaBinPanel.cpp
    panels/CalibrationPanel.hpp
    panels/CalibrationPanel.cpp
    panels/ControlPanel.hpp
    panels/ControlPanel.cpp
    dialogs/PreferencesDialog.hpp
    dialogs/PreferencesDialog.cpp
    dialogs/HapConvertWindow.hpp
    dialogs/HapConvertWindow.cpp
)

target_include_directories(quadmapper_ui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_ui
    PUBLIC
        quadmapper_core
        quadmapper_scene
        quadmapper_renderer
        quadmapper_media
        quadmapper_video
        quadmapper_audio
        quadmapper_output
        quadmapper_source
        quadmapper_input
        quadmapper_control
        imgui
    PRIVATE
        nlohmann_json::nlohmann_json
)

set_target_properties(quadmapper_ui PROPERTIES
    FOLDER "Libraries"
)
