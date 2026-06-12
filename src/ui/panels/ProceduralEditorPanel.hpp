#pragma once

#include "Panel.hpp"
#include "core/UUID.hpp"
#include "core/Types.hpp"
#include <string>

namespace quadmapper {

class SourceManager;
class MediaLibrary;
class ProceduralSource;

/**
 * ProceduralEditorPanel provides a UI for editing procedural source parameters.
 * It displays sliders, color pickers, and toggles based on the source's parameter list.
 */
class ProceduralEditorPanel : public Panel {
public:
    ProceduralEditorPanel();

    /**
     * Set the source manager for accessing procedural sources.
     */
    void setSourceManager(SourceManager* manager) { sourceManager_ = manager; }

    /**
     * Set the media library for accessing media items.
     */
    void setMediaLibrary(MediaLibrary* library) { mediaLibrary_ = library; }

    /**
     * Set the procedural source to edit by media item ID.
     * @param mediaItemId The UUID of the MediaItem
     */
    void setEditingSource(const UUID& mediaItemId);

    /**
     * Clear the current editing source.
     */
    void clearEditingSource();

    /**
     * Check if a source is being edited.
     */
    bool isEditing() const { return !editingMediaItemId_.isNull(); }

    /**
     * Get the ID of the media item being edited.
     */
    const UUID& getEditingMediaItemId() const { return editingMediaItemId_; }

    void render() override;

private:
    // Render the parameter controls for the current source
    void renderParameterControls(ProceduralSource* source);

    // Render a single parameter control
    void renderParameter(ProceduralSource* source, const std::string& name,
                        const std::string& displayName, int type,
                        float currentValue, float minValue, float maxValue);

    // Render resolution controls
    void renderResolutionControls(ProceduralSource* source);

    SourceManager* sourceManager_ = nullptr;
    MediaLibrary* mediaLibrary_ = nullptr;
    UUID editingMediaItemId_;

    // Resolution editing state
    int editWidth_ = 1920;
    int editHeight_ = 1080;
    bool showPresets_ = false;

    // Common resolution presets
    static constexpr int PRESET_COUNT = 6;
    static constexpr int PRESET_WIDTHS[PRESET_COUNT] = { 1920, 1280, 3840, 1024, 2048, 4096 };
    static constexpr int PRESET_HEIGHTS[PRESET_COUNT] = { 1080, 720, 2160, 1024, 2048, 4096 };
    static constexpr const char* PRESET_NAMES[PRESET_COUNT] = {
        "1080p (1920x1080)", "720p (1280x720)", "4K (3840x2160)",
        "1K Square", "2K Square", "4K Square"
    };
};

} // namespace quadmapper
