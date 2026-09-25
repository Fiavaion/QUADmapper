#pragma once

#include "core/Types.hpp"
#include "core/UUID.hpp"

#include <glm/glm.hpp>

namespace quadmapper {

struct EditorContext;
struct TextureBinding;
class Quad;

// Renders the context-sensitive "Properties" dockable window.
// When ctx.es.showProperties is false, render() returns immediately.
// Dispatches to one of three sub-renderers based on ctx.es.currentSelectionType:
//   Quad    -> renderQuadProperties
//   Output  -> renderOutputProperties
//   Media   -> renderMediaProperties
//
// Does NOT inherit from Panel — the Panel::render() signature takes no arguments
// and this panel requires a per-frame EditorContext (same approach as MainMenuBar
// and QuadListPanel). The three static char buffers from the original inline block
// are promoted to member variables to eliminate function-static state.
class InspectorPanel {
public:
    InspectorPanel() = default;

    // Call once per frame inside the ImGui frame, after the dockspace.
    void render(EditorContext& ctx);

private:
    void renderQuadProperties(EditorContext& ctx);
    void renderSimpleQuadProperties(EditorContext& ctx, Quad& quad);  // ADR-042 simple mode
    void renderCropControls(EditorContext& ctx, TextureBinding& texBinding);
    void renderOutputProperties(EditorContext& ctx);
    void renderMediaProperties(EditorContext& ctx);
    void renderObjectProperties(EditorContext& ctx);  // 3D objects (ADR-052)

    char quadName_[128]   = "";
    char outputName_[128] = "";
    char mediaName_[128]  = "";
    char objectName_[128] = "";

    // The selected object's transform as edited: position, rotation (degrees) and
    // uniform scale, re-read from the matrix only when transformShown_ goes stale.
    UUID transformObjectId_;
    glm::mat4 transformShown_{1.0f};
    Vec3 objectPosition_{0.0f};
    Vec3 objectRotationDeg_{0.0f};
    float objectScale_ = 1.0f;

    // Normalized position (0..1) for inserting a grid division (ADR-038 Tier 1).
    float gridInsertPos_ = 0.5f;
};

}  // namespace quadmapper
