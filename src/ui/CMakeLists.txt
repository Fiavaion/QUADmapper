# UI library - Dear ImGui interface

add_library(quadmapper_ui STATIC
    Theme.hpp
    ConfirmPopup.hpp
    Theme.cpp
    AsyncFileDialog.hpp
    AsyncFileDialog.cpp
    ShortcutRouter.hpp
    ShortcutRouter.cpp
    EditorUi.hpp
    EditorUi.cpp
    PanelHost.hpp
    PanelHost.cpp
    panels/Panel.hpp
    panels/ResourcesPanel.hpp
    panels/ResourcesPanel.cpp
    panels/Banners.hpp
    panels/Banners.cpp
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
    panels/AiPhotoPanel.hpp
    panels/AiPhotoPanel.cpp
    dialogs/PreferencesDialog.hpp
    dialogs/PreferencesDialog.cpp
    dialogs/HapConvertWindow.hpp
    dialogs/HapConvertWindow.cpp
    dialogs/MissingMediaWindow.hpp
    dialogs/MissingMediaWindow.cpp
    dialogs/ExportVideoWindow.hpp
    dialogs/ExportVideoWindow.cpp
    dialogs/SessionDialogs.hpp
    dialogs/SessionDialogs.cpp
)

target_include_directories(quadmapper_ui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_ui
    PUBLIC
        quadmapper_editor
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
)

set_target_properties(quadmapper_ui PROPERTIES
    FOLDER "Libraries"
)
