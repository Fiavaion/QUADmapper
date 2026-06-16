#pragma once

#include "Panel.hpp"
#include "core/UUID.hpp"
#include <functional>

namespace quadmapper {

class SourceManager;
class MediaLibrary;
class Quad;
class TextureSource;

/**
 * InspectorPanel displays properties of the currently selected object(s).
 * This includes quad properties, assigned source info, and source controls.
 */
class InspectorPanel : public Panel {
public:
    InspectorPanel();

    void setSourceManager(SourceManager* manager) { sourceManager_ = manager; }
    void setMediaLibrary(MediaLibrary* library) { mediaLibrary_ = library; }

    /**
     * Set the currently selected quad.
     */
    void setSelectedQuad(const UUID& quadId) { selectedQuadId_ = quadId; }
    void clearSelection() { selectedQuadId_ = UUID::null(); }

    /**
     * Callback when "Edit Procedural" is clicked.
     */
    using EditProceduralCallback = std::function<void(const UUID& mediaItemId)>;
    void setEditProceduralCallback(EditProceduralCallback callback) {
        editProceduralCallback_ = std::move(callback);
    }

    void render() override;

private:
    void renderQuadProperties(Quad* quad);
    void renderSourceInfo(Quad* quad);
    void renderVideoControls(TextureSource* source);
    void renderProceduralInfo(TextureSource* source, const UUID& mediaItemId);
    void renderNoSelection();

    SourceManager* sourceManager_ = nullptr;
    MediaLibrary* mediaLibrary_ = nullptr;
    UUID selectedQuadId_;

    EditProceduralCallback editProceduralCallback_;
};

} // namespace quadmapper
