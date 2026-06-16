#pragma once

// EditorState owns the per-session editor/scene state that used to live as ~40 function-
// static "globals" inside Application::render() (see ADR-031, TD-02). Promoting it to an
// owned Application member kills the hidden-global lifetime, removes the dual-track
// SceneManager/static-Scene split, and lets the output render path (TD-01) reach the scene
// without going through render()'s locals. render() aliases each field back to a same-named
// local reference, so the function body is unchanged by the extraction.

#include <unordered_map>
#include <memory>
#include <string>

#include <glad/glad.h>  // GLuint

#include "../core/Types.hpp"   // Vec2
#include "../core/UUID.hpp"
#include "../scene/Scene.hpp"
#include "../scene/QuadManager.hpp"
#include "../renderer/Texture.hpp"
#include "../video/VideoPlayer.hpp"
#include "../ui/panels/InspectorPanel.hpp"
#include "../ui/panels/MainMenuBar.hpp"
#include "../ui/panels/MediaBinPanel.hpp"
#include "../ui/panels/OutputPanel.hpp"
#include "../ui/panels/QuadListPanel.hpp"
#include "../ui/panels/VideoPlayerPanel.hpp"
#include "../ui/panels/ViewportEditor.hpp"

namespace quadmapper {

struct EditorState {
    // Context-sensitive selection tracking
    enum class SelectionType { None, Quad, Output, Media };

    // Scene + quad management
    Scene scene;
    QuadManager& quadManager = scene.getQuadManager();
    std::unordered_map<UUID, std::unique_ptr<Texture>> quadTextures;
    std::unordered_map<UUID, std::string> quadTexturePaths;
    char currentTexturePath[512] = "";
    char projectPath[512] = "project.qmap";
    char statusMessage[256] = "";
    float statusTimer = 0.0f;
    bool initialized = false;
    UUID dragDropQuadId;  // For drag-drop between groups

    // Corner editing state (CORNERS_ONLY mode - the default)
    int cornerHoveredVertex = -1;
    int cornerDraggedVertex = -1;
    int cornerSelectedVertex = -1;  // Persistent selection for keyboard control
    Vec2 cornerDragStartMousePos{0.0f};
    Vec2 cornerDragStartPos{0.0f};

    // Mesh editing state (MESH_EDIT mode)
    int meshHoveredVertex = -1;
    int meshDraggedVertex = -1;
    int meshSelectedVertex = -1;  // Persistent selection for keyboard control
    Vec2 meshDragStartMousePos{0.0f};

    // Snap indicator state (visual feedback when snapping)
    bool snapActive = false;
    Vec2 snapTargetPos{0.0f};
    Vec2 meshDragStartOffset{0.0f};

    // Video player state
    VideoPlayer videoPlayer;
    char videoPath[512] = "";
    GLuint videoTextureId = 0;
    int videoTextureWidth = 0;
    int videoTextureHeight = 0;
    UUID videoAssignedQuadId;  // Which quad has video assigned
    Texture videoTexWrapper;   // Wrapper for the raw video GL texture

    // Panels
    InspectorPanel inspectorPanel;
    MainMenuBar mainMenuBar;
    bool mainMenuBarInitialized = false;
    MediaBinPanel mediaBinPanel;
    bool mediaBinInitialized = false;
    OutputPanel outputPanel;
    bool outputPanelInitialized = false;
    QuadListPanel quadListPanel;
    VideoPlayerPanel videoPlayerPanel;
    ViewportEditor viewportEditor;  // Non-Panel: pre-NewFrame on-canvas warp surface (ADR-0034)

    // Panel visibility toggles
    bool showQuadList = true;
    bool showProperties = true;
    bool showResources = true;
    bool showVideoPlayer = true;

    // Current selection
    SelectionType currentSelectionType = SelectionType::Quad;
};

}  // namespace quadmapper
