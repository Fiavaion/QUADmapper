# UI library - Dear ImGui interface

add_library(quadmapper_ui STATIC
    Theme.hpp
    Theme.cpp
    panels/Panel.hpp
    panels/InspectorPanel.hpp
    panels/InspectorPanel.cpp
    panels/PreviewPanel.hpp
    panels/PreviewPanel.cpp
    panels/OutputPanel.hpp
    panels/OutputPanel.cpp
    panels/MainMenuBar.hpp
    panels/MainMenuBar.cpp
    panels/QuadListPanel.hpp
    panels/QuadListPanel.cpp
    panels/MediaBinPanel.hpp
    panels/MediaBinPanel.cpp
    panels/ProceduralEditorPanel.hpp
    panels/ProceduralEditorPanel.cpp
    dialogs/PreferencesDialog.hpp
    dialogs/PreferencesDialog.cpp
    dialogs/OutputConfigDialog.hpp
    dialogs/OutputConfigDialog.cpp
    widgets/QuadCornerWidget.hpp
    widgets/QuadCornerWidget.cpp
    widgets/PlaybackControls.hpp
    widgets/PlaybackControls.cpp
    widgets/MediaThumbnail.hpp
    widgets/MediaThumbnail.cpp
    menus/MainMenu.hpp
    menus/MainMenu.cpp
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
        imgui
    PRIVATE
        nlohmann_json::nlohmann_json
)

set_target_properties(quadmapper_ui PROPERTIES
    FOLDER "Libraries"
)
