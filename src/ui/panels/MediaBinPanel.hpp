#pragma once

#include "Panel.hpp"
#include "ui/AsyncFileDialog.hpp"
#include "core/UUID.hpp"
#include "core/Types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <functional>
#include <memory>
#include <filesystem>

namespace quadmapper {

class MediaLibrary;
class MediaItem;
class HapConvertWindow;
class SourceManager;
class ThumbnailCache;
class Texture;

/**
 * MediaBinPanel displays imported media items in a thumbnail grid.
 * Users can import, remove, and drag media items to assign them to quads.
 * Also supports creating procedural texture sources.
 */
class MediaBinPanel : public Panel {
public:
    MediaBinPanel();
    ~MediaBinPanel();  // defined in .cpp (owns unique_ptr<Texture> video thumbnails)

    /**
     * Set the media library to display.
     * @param library Pointer to the media library
     */
    void setMediaLibrary(MediaLibrary* library) { mediaLibrary_ = library; }
    const MediaLibrary* getMediaLibrary() const { return mediaLibrary_; }

    /**
     * Set the source manager for creating procedural sources.
     * @param manager Pointer to the source manager
     */
    void setSourceManager(SourceManager* manager) { sourceManager_ = manager; }

    /**
     * Set the thumbnail cache for rendering media thumbnails.
     * @param cache Pointer to the thumbnail cache
     */
    void setThumbnailCache(ThumbnailCache* cache) { thumbnailCache_ = cache; }

    /**
     * Render the panel.
     */
    void render() override;

    /**
     * Render just the panel content (without window wrapper).
     * Useful for embedding in tabs.
     */
    void renderContent();

    /**
     * Get the currently selected media item ID.
     * @return The UUID of the selected item, or null UUID if none selected
     */
    const UUID& getSelectedItemId() const { return selectedItemId_; }

    /**
     * Check if there is a selected item.
     */
    bool hasSelection() const { return !selectedItemId_.isNull(); }

    /**
     * Clear the current selection.
     */
    void clearSelection() { selectedItemId_ = UUID::null(); }

    /**
     * The hover-to-play preview source, or null. It is bound to no quad, so the
     * unused-source cleanup must be told to keep it.
     */
    const UUID& getPreviewSourceId() const { return hoverPreviewId_; }

    /**
     * The preview's decoder was bound elsewhere (a Load or an assignment found it by
     * path, like any open clip): it belongs to that user now. Forget it without
     * removing it; a still-hovered item gets a new preview next frame.
     */
    void handOverPreviewSource() {
        hoverPreviewId_ = UUID();
        hoverPreviewPath_.clear();
    }

    /**
     * Previews whose hover ended since the last call. The bin never removes a preview
     * itself: an assignment earlier in the same frame may have bound it. The frame
     * boundary (releaseUnusedSources) removes each one nothing bound.
     */
    std::vector<UUID> takeEndedPreviews() { return std::exchange(endedPreviews_, {}); }

    // Thumbnail size settings
    float getThumbnailSize() const { return thumbnailSize_; }
    void setThumbnailSize(float size) { thumbnailSize_ = size; }

    // Callback when a procedural source is selected for editing
    using ProceduralEditCallback = std::function<void(const UUID&)>;
    void setProceduralEditCallback(ProceduralEditCallback callback) { proceduralEditCallback_ = std::move(callback); }

    // Convert to HAP (QM-082): the progress window for conversions started from the
    // context menu. Drawn every frame, whichever tab is showing.
    void renderConversions();

    // "Replace in show" on a finished conversion: moves what shows the original clip
    // onto the converted file. Returns why not, or empty.
    using ReplaceInShowCallback = std::function<std::string(const std::filesystem::path& original,
                                                            const std::filesystem::path& converted)>;
    void setReplaceInShowCallback(ReplaceInShowCallback callback) { replaceInShowCallback_ = std::move(callback); }

private:
    // Render the import button and controls
    void renderControls();

    // Render the procedural creation menu
    void renderProceduralMenu();

    // Render the thumbnail grid
    void renderThumbnailGrid();

    // Render a single media item thumbnail
    void renderMediaItem(MediaItem* item, int index);

    // Render the context menu for a media item
    void renderItemContextMenu(MediaItem* item);

    // Handle import button click
    void handleImport();

    // Handle "Load Folder" — pick a folder and scan all media in it (ADR-042 P3)
    void handleImportFolder();

    // Add whatever the Load Files / Load Folder dialog returned, once it has closed.
    void pollImportDialog();

    // Handle procedural creation
    void createProcedural(ProceduralType type);

    // Build the file filter string for native file dialog
    std::string buildFileFilter() const;

    // Get display icon for media type
    const char* getMediaTypeIcon(MediaType type) const;

    // Lazily extract + cache a video's first frame as a standalone thumbnail texture
    // (one-shot decode, no persistent decoder). Returns nullptr on failure.
    Texture* getVideoThumbnail(const std::filesystem::path& path);

    // Hover-to-play: after each grid pass, switch/teardown the single dedicated
    // preview decoder so the video under the cursor plays (and reverts when you leave).
    void updateHoverPreview();

    MediaLibrary* mediaLibrary_ = nullptr;
    SourceManager* sourceManager_ = nullptr;
    ThumbnailCache* thumbnailCache_ = nullptr;
    UUID selectedItemId_;

    // UI settings
    float thumbnailSize_ = 80.0f;
    float thumbnailSpacing_ = 8.0f;

    // Drag-drop payload type identifier
    static constexpr const char* DRAG_DROP_MEDIA_ID = "MEDIA_ITEM_ID";

    // Context menu state
    UUID contextMenuItemId_;

    // Procedural edit callback
    ProceduralEditCallback proceduralEditCallback_;

    // Convert to HAP conversions and their window (created on first use)
    std::unique_ptr<HapConvertWindow> hapConvert_;
    ReplaceInShowCallback replaceInShowCallback_;

    // Search and filter state
    char searchFilter_[256] = "";
    int typeFilterIndex_ = 0;  // 0 = All, 1 = Images, 2 = Videos, 3 = Procedural

    // Cached video first-frame thumbnails, keyed by file path (RAII textures).
    std::unordered_map<std::string, std::unique_ptr<Texture>> videoThumbs_;

    // Hover-to-play preview: ONE dedicated decoder for the video under the cursor.
    // Kept separate (createVideoSource, not getOrCreate) so it never disturbs a video
    // already assigned to a quad. hoveredVideoThisFrame_ is collected during the grid
    // pass, then reconciled in updateHoverPreview().
    UUID hoverPreviewId_;
    std::string hoverPreviewPath_;
    std::string hoveredVideoThisFrame_;
    std::vector<UUID> endedPreviews_;  // see takeEndedPreviews()

    // Load Files / Load Folder run off the render thread (QM-088); one at a time.
    AsyncFileDialog importDialog_;
    bool importIsFolder_ = false;
};

} // namespace quadmapper
