#pragma once

#include "Panel.hpp"
#include "ui/AsyncFileDialog.hpp"
#include "core/UUID.hpp"
#include "ui/ConfirmPopup.hpp"
#include "core/Types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <memory>
#include <filesystem>

namespace quadmapper {

struct EditorContext;
struct EditorState;
class MediaLibrary;
class MediaItem;
class HapConvertWindow;
class SourceManager;
class ThumbnailCache;
class Texture;
class Scene;

// How many quads and 3D objects, in every scene, are bound to the media source.
size_t countMediaUsers(const std::vector<std::unique_ptr<Scene>>& scenes, const UUID& sourceId);

/**
 * The Media tab of the Resources window: imported media items in a thumbnail grid.
 * Users can import, remove, and drag media items to assign them to quads.
 * Also supports creating procedural texture sources. The selected item and the hover
 * preview live in EditorState (selectedMediaId, previewSourceId).
 */
class MediaBinPanel : public Panel {
public:
    static constexpr const char* ID = "Media";

    MediaBinPanel();
    ~MediaBinPanel() override;  // defined in .cpp (owns unique_ptr<Texture> video thumbnails)

    const char* id() const override { return ID; }
    PanelKind kind() const override { return PanelKind::Tab; }
    // Keeps the editor state, the media library, the sources and the thumbnail cache.
    void onAttach(EditorContext& ctx) override;
    // Selecting an item shows it in the Properties window.
    void drawContent(EditorContext& ctx) override;

    // Convert to HAP (QM-082): the progress window for conversions started from the
    // context menu, with "Replace in show" on a finished one. Drawn every frame,
    // whichever tab is showing (EditorUi draws it as an overlay).
    void renderConversions();

private:
    void renderContent();

    void renderControls();

    void renderProceduralMenu();

    void renderThumbnailGrid();

    void renderMediaItem(MediaItem* item, int index);

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

    const char* getMediaTypeIcon(MediaType type) const;

    // Lazily extract + cache a video's first frame as a standalone thumbnail texture
    // (one-shot decode, no persistent decoder). Returns nullptr on failure.
    Texture* getVideoThumbnail(const std::filesystem::path& path);

    // Removing a library item: the context menu records the request, the grid loop
    // finishes, then the item is removed (asking first when quads use it).
    void processRemoveRequest();
    void renderRemoveConfirm();
    void removeItem(const UUID& id);

    // Hover-to-play: after each grid pass, switch/teardown the single dedicated
    // preview decoder so the video under the cursor plays (and reverts when you leave).
    void updateHoverPreview();

    EditorState* es_ = nullptr;
    MediaLibrary* mediaLibrary_ = nullptr;
    SourceManager* sourceManager_ = nullptr;
    ThumbnailCache* thumbnailCache_ = nullptr;

    // UI settings
    float thumbnailSize_ = 80.0f;
    float thumbnailSpacing_ = 8.0f;

    // Drag-drop payload type identifier
    static constexpr const char* DRAG_DROP_MEDIA_ID = "MEDIA_ITEM_ID";

    // Context menu state
    UUID contextMenuItemId_;

    ConfirmPopup removeConfirm_{"mediaremove"};
    UUID removeRequestId_;  // set by the context menu, handled after the grid loop
    UUID pendingRemoveId_;  // the item the open confirmation is about

    // New thumbnails built so far this frame (image or video); reset by the grid pass.
    int thumbsBuiltThisFrame_ = 0;
    static constexpr int MAX_THUMBS_PER_FRAME = 1;

    // Procedural edit callback

    // Convert to HAP conversions and their window (created on first use)
    std::unique_ptr<HapConvertWindow> hapConvert_;

    // Search and filter state
    char searchFilter_[256] = "";
    int typeFilterIndex_ = 0;  // 0 = All, 1 = Images, 2 = Videos, 3 = Procedural

    // Cached video first-frame thumbnails, keyed by file path (RAII textures).
    std::unordered_map<std::string, std::unique_ptr<Texture>> videoThumbs_;

    // Hover-to-play preview: ONE dedicated decoder for the video under the cursor
    // (es_.previewSourceId). Kept separate (createVideoSource, not getOrCreate) so it
    // never disturbs a video already assigned to a quad. hoveredVideoThisFrame_ is
    // collected during the grid pass, then reconciled in updateHoverPreview().
    std::string hoveredVideoThisFrame_;

    // Load Files / Load Folder run off the render thread (QM-088); one at a time.
    AsyncFileDialog importDialog_;
    bool importIsFolder_ = false;
};

} // namespace quadmapper
