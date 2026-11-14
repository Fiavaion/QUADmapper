#pragma once

#include "Panel.hpp"
#include "SliceEditor.hpp"
#include "core/Types.hpp"
#include "core/UUID.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <string>

namespace quadmapper {

struct EditorContext;
class Quad;

// The context-sensitive "Properties" dockable window. Dispatches on
// ctx.es.currentSelectionType to the quad, output, media or object properties.
class InspectorPanel : public Panel {
public:
    static constexpr const char* ID = "Properties";

    InspectorPanel() = default;

    const char* id() const override { return ID; }
    void drawContent(EditorContext& ctx) override;

private:
    void renderQuadProperties(EditorContext& ctx);
    void renderSimpleQuadProperties(EditorContext& ctx, Quad& quad);  // ADR-042 simple mode
    void renderCropControls(EditorContext& ctx, Quad& quad);
    void renderOutputProperties(EditorContext& ctx);
    void renderMediaProperties(EditorContext& ctx);
    void renderObjectProperties(EditorContext& ctx);  // 3D objects (ADR-052)

    SliceEditor sliceEditor_;  // the selected output's slices (ADR-047)

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

    // The quad and media path the texture Path field last showed (followTexturePath).
    UUID pathFieldQuad_;
    std::string pathFieldPath_;

    // The undo step the active quad drag or slider records when it is released (one
    // drag is one step); empty while none is active. pendingStepQuad_ is the quad the
    // Properties showed last frame, the one such a step belongs to.
    std::function<void()> pendingStep_;
    UUID pendingStepQuad_;
};

}  // namespace quadmapper
