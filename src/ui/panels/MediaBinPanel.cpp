#include "MediaBinPanel.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/PerformLock.hpp"
#include "editor/ReplaceMedia.hpp"
#include "core/FileSystem.hpp"
#include "media/MediaLibrary.hpp"
#include "media/MediaItem.hpp"
#include "media/ThumbnailCache.hpp"
#include "scene/Scene.hpp"
#include "scene/QuadManager.hpp"
#include "scene/Quad.hpp"
#include "scene/MeshObject.hpp"
#include "source/SourceManager.hpp"
#include "source/procedural/ProceduralFactory.hpp"
#include "renderer/Texture.hpp"
#include "ui/dialogs/HapConvertWindow.hpp"
#include "video/VideoDecoder.hpp"
#include "ui/ConfirmPopup.hpp"
#include <imgui.h>
#include <vector>
#include <algorithm>
#include <glad/glad.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace quadmapper {

// Helper to get display name for procedural types
static const char* getProceduralTypeName(ProceduralType type) {
    switch (type) {
        case ProceduralType::SOLID_COLOR: return "Solid Color";
        case ProceduralType::GRADIENT: return "Gradient";
        case ProceduralType::CHECKERBOARD: return "Checkerboard";
        case ProceduralType::TEST_PATTERN: return "Test Pattern";
        case ProceduralType::NOISE: return "Noise";
        case ProceduralType::PLASMA: return "Plasma";
        case ProceduralType::GRID: return "Grid";
        default: return "Unknown";
    }
}

MediaBinPanel::MediaBinPanel() {
    contextMenuItemId_ = UUID::null();
}

// Defined here (not in the header) so unique_ptr<Texture> sees the complete type.
MediaBinPanel::~MediaBinPanel() = default;

Texture* MediaBinPanel::getVideoThumbnail(const std::filesystem::path& path) {
    const std::string key = utf8FromPath(path);
    auto it = videoThumbs_.find(key);
    if (it != videoThumbs_.end()) return it->second.get();

    // One new thumbnail per frame: the render thread also drives the projector outputs.
    if (thumbsBuiltThisFrame_ >= MAX_THUMBS_PER_FRAME) return nullptr;
    ++thumbsBuiltThisFrame_;

    // Cache a null entry first so a file that fails to decode isn't retried each frame.
    videoThumbs_[key] = nullptr;

    VideoDecoder decoder;
    if (!decoder.open(path)) return nullptr;
    VideoFrame frame = decoder.decodeNextFrame();  // first frame; no background thread
    decoder.close();
    if (!frame.isValid()) return nullptr;

    // RGB24 -> RGBA so the GL upload is alignment-safe for any width.
    const int w = frame.width, h = frame.height;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        rgba[i * 4 + 0] = frame.data[i * 3 + 0];
        rgba[i * 4 + 1] = frame.data[i * 3 + 1];
        rgba[i * 4 + 2] = frame.data[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
    auto tex = std::make_unique<Texture>();
    if (!tex->create(w, h, 4, rgba.data())) return nullptr;
    Texture* raw = tex.get();
    videoThumbs_[key] = std::move(tex);
    return raw;
}

void MediaBinPanel::onAttach(EditorContext& ctx) {
    es_ = &ctx.es;
    mediaLibrary_ = ctx.mediaLibrary;
    sourceManager_ = ctx.sourceManager;
    thumbnailCache_ = ctx.thumbnails;
}

void MediaBinPanel::drawContent(EditorContext& ctx) {
    EditorState& es = ctx.es;
    const UUID prevMediaSelection = es.selectedMediaId;
    renderContent();
    // Switch to Media properties if a media item was just selected
    if (!es.selectedMediaId.isNull() && es.selectedMediaId != prevMediaSelection) {
        es.currentSelectionType = EditorState::SelectionType::Media;
        es.quadManager().deselectAll();
        es.selectedOutputId = UUID();
    }
}

void MediaBinPanel::renderContent() {
    if (!mediaLibrary_ || !es_) {
        ImGui::TextDisabled("No media library connected");
        return;
    }

    pollImportDialog();
    renderControls();
    ImGui::Separator();
    renderThumbnailGrid();
    processRemoveRequest();
    renderRemoveConfirm();
}

size_t countMediaUsers(const std::vector<std::unique_ptr<Scene>>& scenes, const UUID& sourceId) {
    if (sourceId.isNull()) return 0;
    size_t users = 0;
    for (const auto& scene : scenes) {
        if (!scene) continue;
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            if (quad->getTexture().mediaId == sourceId) ++users;
        }
        for (const auto& object : scene->getObjects()) {
            if (object->getTexture().mediaId == sourceId) ++users;
        }
    }
    return users;
}

// The source the quads bind for this item: a procedural item owns one; an image or
// video gets it from the SourceManager, once it has been assigned.
static UUID sourceIdOf(const MediaItem& item, const SourceManager* sources) {
    if (item.hasSource()) return item.getSourceId();
    if (!sources) return UUID();
    if (item.getType() == MediaType::IMAGE) return sources->findImageSourceByPath(item.getPath());
    if (item.getType() == MediaType::VIDEO) return sources->findVideoSourceByPath(item.getPath());
    return UUID();
}

// "Remove from Library" only records the request: removing the item inside its own
// context menu would free it while the menu still reads it. An item no quad or object
// uses goes at once; one in use asks first.
void MediaBinPanel::processRemoveRequest() {
    if (removeRequestId_.isNull()) return;
    const UUID id = removeRequestId_;
    removeRequestId_ = UUID::null();
    const MediaItem* item = mediaLibrary_->getItem(id);
    if (!item) return;

    const size_t users = countMediaUsers(es_->scenes, sourceIdOf(*item, sourceManager_));
    if (users == 0) {
        removeItem(id);
        return;
    }
    pendingRemoveId_ = id;
    removeConfirm_.request(
        "Remove Media?",
        "'" + item->getName() + "' is used by " + std::to_string(users) +
            (users == 1 ? " quad" : " quads") + ". Removing it leaves them without media.",
        "Remove");
}

void MediaBinPanel::renderRemoveConfirm() {
    if (!removeConfirm_.draw()) return;
    const UUID id = pendingRemoveId_;
    pendingRemoveId_ = UUID::null();
    removeItem(id);
}

void MediaBinPanel::removeItem(const UUID& id) {
    const MediaItem* item = mediaLibrary_->getItem(id);
    if (!item) return;
    if (es_->selectedMediaId == id) es_->selectedMediaId = UUID::null();
    // Also remove the source from SourceManager if it exists
    if (sourceManager_ && item->hasSource()) {
        sourceManager_->removeSource(item->getSourceId());
    }
    mediaLibrary_->removeItem(id);
}

void MediaBinPanel::renderControls() {
    // Load files into the bin (images, videos). Renamed from "Import..." so the
    // primary load action reads plainly for new users (ADR-042 P3).
    if (ImGui::Button("Load Files...")) {
        handleImport();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Browse for media files (images, videos)");
    }

    ImGui::SameLine();

    // Load every supported file in a folder (and its subfolders).
    if (ImGui::Button("Load Folder...")) {
        handleImportFolder();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add all media in a folder (and subfolders)");
    }

    ImGui::SameLine();

    // Create procedural button with dropdown
    if (ImGui::Button("+ Create")) {
        ImGui::OpenPopup("ProceduralMenu");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create procedural texture source");
    }

    // Render the procedural creation menu
    renderProceduralMenu();

    // Second row: search and filter
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##search", "Search...", searchFilter_, sizeof(searchFilter_));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Search media by name");
    }

    ImGui::SameLine();

    // Type filter dropdown
    ImGui::SetNextItemWidth(90.0f);
    const char* typeLabels[] = { "All", "Images", "Videos", "Procedural" };
    ImGui::Combo("##typeFilter", &typeFilterIndex_, typeLabels, IM_ARRAYSIZE(typeLabels));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Filter by media type");
    }

    ImGui::SameLine();

    // Thumbnail size slider
    ImGui::SetNextItemWidth(80.0f);
    ImGui::SliderFloat("##size", &thumbnailSize_, 48.0f, 128.0f, "%.0f px");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Thumbnail size");
    }

    ImGui::SameLine();

    // Item count
    ImGui::TextDisabled("(%zu items)", mediaLibrary_->getItemCount());
}

void MediaBinPanel::renderThumbnailGrid() {
    const auto& items = mediaLibrary_->getItems();
    if (items.empty()) {
        ImGui::TextDisabled("No media imported.");
        ImGui::TextDisabled("Click 'Load Files...' or 'Load Folder...' to add media.");
        ImGui::TextDisabled("Or drag files here from Explorer.");
        return;
    }

    // Build filtered list
    std::vector<MediaItem*> filteredItems;
    std::string searchLower(searchFilter_);
    std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& item : items) {
        // Type filter
        if (typeFilterIndex_ != 0) {
            MediaType requiredType = MediaType::IMAGE;
            switch (typeFilterIndex_) {
                case 1: requiredType = MediaType::IMAGE; break;
                case 2: requiredType = MediaType::VIDEO; break;
                case 3: requiredType = MediaType::PROCEDURAL; break;
            }
            if (item->getType() != requiredType) continue;
        }

        // Search filter
        if (!searchLower.empty()) {
            std::string nameLower = item->getName();
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (nameLower.find(searchLower) == std::string::npos) continue;
        }

        filteredItems.push_back(item.get());
    }

    // Show filter status if filtering
    if (filteredItems.size() != items.size()) {
        ImGui::TextDisabled("Showing %zu of %zu", filteredItems.size(), items.size());
    }

    if (filteredItems.empty()) {
        ImGui::TextDisabled("No items match filter.");
        return;
    }

    // Calculate grid layout
    float panelWidth = ImGui::GetContentRegionAvail().x;
    float cellSize = thumbnailSize_ + thumbnailSpacing_;
    int columns = std::max(1, static_cast<int>(panelWidth / cellSize));

    ImGui::BeginChild("MediaGrid", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

    // Collect which video (if any) is hovered this pass; reconciled after the loop.
    hoveredVideoThisFrame_.clear();
    thumbsBuiltThisFrame_ = 0;

    int index = 0;
    for (MediaItem* item : filteredItems) {
        // Start new row if needed
        if (index > 0 && index % columns != 0) {
            ImGui::SameLine();
        }

        renderMediaItem(item, index);
        index++;
    }

    ImGui::EndChild();

    updateHoverPreview();
}

void MediaBinPanel::updateHoverPreview() {
    if (!sourceManager_) return;
    // Only act when the hovered video changes (avoid per-frame decoder churn).
    if (hoveredVideoThisFrame_ == es_->previewSourcePath) return;

    // Retire the previous preview decoder; the frame boundary removes it unless
    // something bound it meanwhile (releaseUnusedSources).
    if (!es_->previewSourceId.isNull()) {
        es_->endedPreviews.push_back(es_->previewSourceId);
        es_->previewSourceId = UUID();
        es_->previewSourcePath.clear();
    }

    // Spin up a dedicated decoder for the newly-hovered video and play it. This is a
    // SEPARATE source (createVideoSource, not getOrCreate) so it never touches the
    // playback of a video already bound to a quad.
    if (!hoveredVideoThisFrame_.empty()) {
        UUID sid = sourceManager_->createVideoSource(pathFromUtf8(hoveredVideoThisFrame_));
        if (!sid.isNull()) {
            es_->previewSourceId = sid;
            es_->previewSourcePath = hoveredVideoThisFrame_;
            if (auto* v = sourceManager_->getSourceAs<PlayableSource>(sid)) v->play();
        }
    }
}

void MediaBinPanel::renderMediaItem(MediaItem* item, int index) {
    ImGui::PushID(index);

    bool isSelected = (item->getId() == es_->selectedMediaId);
    ImVec2 size(thumbnailSize_, thumbnailSize_);

    // Create a button-like area for the thumbnail
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();

    // Background color based on selection
    ImU32 bgColor = isSelected ?
        ImGui::GetColorU32(ImGuiCol_ButtonActive) :
        ImGui::GetColorU32(ImGuiCol_Button);

    ImGui::GetWindowDrawList()->AddRectFilled(
        cursorPos,
        ImVec2(cursorPos.x + size.x, cursorPos.y + size.y),
        bgColor,
        4.0f  // Rounded corners
    );

    // Invisible button for interaction
    ImGui::InvisibleButton("##thumb", size);

    // Handle selection on click
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        es_->selectedMediaId = item->getId();
    }

    // Handle right-click for context menu
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        es_->selectedMediaId = item->getId();
        contextMenuItemId_ = item->getId();
        ImGui::OpenPopup("MediaItemContextMenu");
    }

    // Drag source for drag-drop to quads
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        // Store the UUID as payload
        UUID id = item->getId();
        ImGui::SetDragDropPayload(DRAG_DROP_MEDIA_ID, &id, sizeof(UUID));

        // Show drag preview
        ImGui::Text("Assign: %s", item->getName().c_str());
        ImGui::EndDragDropSource();
    }

    // Hover highlight
    if (ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRect(
            cursorPos,
            ImVec2(cursorPos.x + size.x, cursorPos.y + size.y),
            ImGui::GetColorU32(ImGuiCol_ButtonHovered),
            4.0f,
            0,
            2.0f
        );
    }

    // Draw thumbnail image or placeholder icon
    bool thumbnailDrawn = false;

    // A cell scrolled out of view asks for nothing.
    const bool cellVisible =
        ImGui::IsRectVisible(cursorPos, ImVec2(cursorPos.x + size.x, cursorPos.y + size.y));

    // Try to get a real thumbnail for image items. A new one is built at most
    // MAX_THUMBS_PER_FRAME times a frame; cells still waiting show the placeholder.
    if (thumbnailCache_ && cellVisible && item->getType() == MediaType::IMAGE) {
        GLuint thumbTex = 0;
        bool mayBuild = true;
        if (thumbnailCache_->needsGeneration(item->getPath())) {
            mayBuild = thumbsBuiltThisFrame_ < MAX_THUMBS_PER_FRAME;
            if (mayBuild) ++thumbsBuiltThisFrame_;
        }
        if (mayBuild) {
            thumbTex = thumbnailCache_->getThumbnail(item->getPath(),
                                                     static_cast<int>(thumbnailSize_));
        }
        if (thumbTex != 0) {
            // Calculate aspect-fit placement within the thumbnail area
            // For now, just center the thumbnail
            ImVec2 thumbPos(cursorPos.x + 2.0f, cursorPos.y + 2.0f);
            ImVec2 thumbSize(size.x - 4.0f, size.y - 20.0f);  // Leave room for name

            ImGui::GetWindowDrawList()->AddImage(
                (ImTextureID)(uintptr_t)thumbTex,
                thumbPos,
                ImVec2(thumbPos.x + thumbSize.x, thumbPos.y + thumbSize.y),
                ImVec2(0, 0),  // UV min
                ImVec2(1, 1),  // UV max
                IM_COL32(255, 255, 255, 255)
            );
            thumbnailDrawn = true;
        }
    }

    // Procedural sources are GPU-native with a live texture — draw it directly so the
    // bin shows the actual generated pattern instead of a [GEN] placeholder (ADR-042).
    if (!thumbnailDrawn && sourceManager_ &&
        item->getType() == MediaType::PROCEDURAL && item->hasSource()) {
        if (Texture* tex = sourceManager_->getTexture(item->getSourceId())) {
            if (tex->isValid()) {
                ImVec2 thumbPos(cursorPos.x + 2.0f, cursorPos.y + 2.0f);
                ImVec2 thumbSize(size.x - 4.0f, size.y - 20.0f);
                ImGui::GetWindowDrawList()->AddImage(
                    (ImTextureID)(uintptr_t)tex->getId(),
                    thumbPos,
                    ImVec2(thumbPos.x + thumbSize.x, thumbPos.y + thumbSize.y),
                    ImVec2(0, 0), ImVec2(1, 1),
                    IM_COL32(255, 255, 255, 255));
                thumbnailDrawn = true;
            }
        }
    }

    // Video: show the first frame (lazily extracted + cached). On hover, play it —
    // record the hover so updateHoverPreview() spins up the dedicated decoder, and
    // draw that live texture instead of the static first frame.
    if (!thumbnailDrawn && item->getType() == MediaType::VIDEO) {
        const std::string path = utf8FromPath(item->getPath());
        if (ImGui::IsItemHovered()) hoveredVideoThisFrame_ = path;

        Texture* drawTex = nullptr;
        if (path == es_->previewSourcePath && sourceManager_ && !es_->previewSourceId.isNull()) {
            drawTex = sourceManager_->getTexture(es_->previewSourceId);  // live preview
        }
        if ((!drawTex || !drawTex->isValid()) && cellVisible) {
            drawTex = getVideoThumbnail(item->getPath());           // static first frame
        }
        if (drawTex && drawTex->isValid()) {
            ImVec2 thumbPos(cursorPos.x + 2.0f, cursorPos.y + 2.0f);
            ImVec2 thumbSize(size.x - 4.0f, size.y - 20.0f);
            ImGui::GetWindowDrawList()->AddImage(
                (ImTextureID)(uintptr_t)drawTex->getId(),
                thumbPos,
                ImVec2(thumbPos.x + thumbSize.x, thumbPos.y + thumbSize.y),
                ImVec2(0, 0), ImVec2(1, 1),
                IM_COL32(255, 255, 255, 255));
            thumbnailDrawn = true;
        }
    }

    // Fall back to type icon if no thumbnail
    if (!thumbnailDrawn) {
        ImVec2 iconPos(cursorPos.x + size.x * 0.5f, cursorPos.y + size.y * 0.35f);
        const char* typeIcon = getMediaTypeIcon(item->getType());
        ImVec2 textSize = ImGui::CalcTextSize(typeIcon);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(iconPos.x - textSize.x * 0.5f, iconPos.y - textSize.y * 0.5f),
            ImGui::GetColorU32(ImGuiCol_Text),
            typeIcon
        );
    }

    // Draw filename below the icon (truncated if too long)
    std::string name = item->getName();
    float maxTextWidth = size.x - 4.0f;
    ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());

    // Truncate with ellipsis if too long
    if (nameSize.x > maxTextWidth) {
        while (name.length() > 3 && ImGui::CalcTextSize((name + "...").c_str()).x > maxTextWidth) {
            name.pop_back();
        }
        name += "...";
        nameSize = ImGui::CalcTextSize(name.c_str());
    }

    ImVec2 namePos(
        cursorPos.x + (size.x - nameSize.x) * 0.5f,
        cursorPos.y + size.y * 0.65f
    );
    ImGui::GetWindowDrawList()->AddText(namePos, ImGui::GetColorU32(ImGuiCol_Text), name.c_str());

    // Selection outline
    if (isSelected) {
        ImGui::GetWindowDrawList()->AddRect(
            cursorPos,
            ImVec2(cursorPos.x + size.x, cursorPos.y + size.y),
            ImGui::GetColorU32(ImGuiCol_ButtonActive),
            4.0f,
            0,
            3.0f
        );
    }

    // Tooltip with full info
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("%s", item->getName().c_str());
        if (item->getType() == MediaType::PROCEDURAL) {
            ImGui::TextDisabled("Type: Procedural (%s)", getProceduralTypeName(item->getProceduralType()));
            ImGui::TextDisabled("Double-click to edit parameters");
        } else {
            ImGui::TextDisabled("Path: %s", utf8FromPath(item->getPath()).c_str());
            ImGui::TextDisabled("Type: %s", item->getType() == MediaType::VIDEO ? "Video" : "Image");
        }
        ImGui::TextDisabled("Drag to assign to a quad");
        ImGui::EndTooltip();
    }

    // Handle double-click for procedural editing
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (item->getType() == MediaType::PROCEDURAL && proceduralEditCallback_) {
            proceduralEditCallback_(item->getId());
        }
    }

    // Context menu
    renderItemContextMenu(item);

    ImGui::PopID();
}

void MediaBinPanel::renderItemContextMenu(MediaItem* item) {
    if (ImGui::BeginPopup("MediaItemContextMenu")) {
        if (contextMenuItemId_ != item->getId()) {
            ImGui::EndPopup();
            return;
        }

        ImGui::TextDisabled("%s", item->getName().c_str());
        ImGui::Separator();

        // Procedural-specific options
        if (item->getType() == MediaType::PROCEDURAL) {
            if (ImGui::MenuItem("Edit Parameters...")) {
                if (proceduralEditCallback_) {
                    proceduralEditCallback_(item->getId());
                }
            }
            ImGui::Separator();
        }

        if (ImGui::MenuItem("Remove from Library")) {
            removeRequestId_ = item->getId();
        }

        // Re-encode as HAP beside the original (QM-082); runs in the background.
        if (item->getType() == MediaType::VIDEO) {
            ImGui::Separator();
            for (const auto variant : {hap::HapVariant::Hap, hap::HapVariant::HapQ}) {
                const bool busy = hapConvert_ && hapConvert_->isConverting(item->getPath(), variant);
                const char* label = variant == hap::HapVariant::Hap ? "Convert to HAP..." : "Convert to HAP Q...";
                if (ImGui::MenuItem(label, busy ? "converting" : nullptr, false, !busy)) {
                    if (!hapConvert_) hapConvert_ = std::make_unique<HapConvertWindow>();
                    hapConvert_->start(item->getPath(), variant);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", variant == hap::HapVariant::Hap
                                                ? "Write a HAP copy beside this file: GPU-ready frames that decode\n"
                                                  "cheaply, so many clips play at once and Ping-Pong runs smoothly."
                                                : "Write a HAP Q copy beside this file: like HAP with truer colour,\n"
                                                  "at about twice the file size.");
                }
            }
        }

#ifdef _WIN32
        // Only show file-related options for non-procedural items
        if (item->getType() != MediaType::PROCEDURAL) {
            if (ImGui::MenuItem("Reveal in Explorer")) {
                std::wstring path = item->getPath().wstring();
                // No file name holds a quote; one from a project file would end the
                // quoted path and add an Explorer switch.
                if (path.find(L'"') == std::wstring::npos) {
                    std::wstring param = L"/select,\"" + path + L"\"";
                    ShellExecuteW(NULL, L"open", L"explorer.exe", param.c_str(), NULL, SW_SHOWNORMAL);
                }
            }

            if (ImGui::MenuItem("Copy Path")) {
                ImGui::SetClipboardText(utf8FromPath(item->getPath()).c_str());
            }
        }
#endif

        ImGui::EndPopup();
    }
}

void MediaBinPanel::renderConversions() {
    if (!hapConvert_) return;
    hapConvert_->render(
        [this](const std::filesystem::path& converted) {
            if (mediaLibrary_) mediaLibrary_->addItem(converted);  // null if already there
        },
        [this](const std::filesystem::path& original, const std::filesystem::path& converted) {
            if (!sourceManager_ || !es_) return std::string("Media cannot be loaded right now.");
            if (refusedByPerformLock(*es_, "Replace in show")) {
                return std::string("The show is locked (Output > Perform Lock).");
            }
            return replaceClipInShow(*es_, *sourceManager_, original, converted);
        },
        es_ && es_->performLock.isOn());
}

void MediaBinPanel::handleImport() {
    // Runs off the render thread (QM-088); pollImportDialog() adds the chosen files.
    AsyncFileDialog::Request request;
    request.kind = AsyncFileDialog::Kind::OpenFiles;
    request.title = "Import Media Files";
    request.filter = buildFileFilter();
    if (importDialog_.open(std::move(request))) importIsFolder_ = false;
}

void MediaBinPanel::handleImportFolder() {
    // Recursively scans every supported media file in the chosen folder into the
    // library (MediaLibrary::scanFolder), once pollImportDialog() sees the result.
    AsyncFileDialog::Request request;
    request.kind = AsyncFileDialog::Kind::PickFolder;
    request.title = "Select a folder to load all media from";
    if (importDialog_.open(std::move(request))) importIsFolder_ = true;
}

void MediaBinPanel::pollImportDialog() {
    std::vector<std::string> chosen;
    if (!importDialog_.takeResult(chosen)) return;
    for (const std::string& path : chosen) {
        if (importIsFolder_) {
            mediaLibrary_->scanFolder(pathFromUtf8(path), /*recursive=*/true);
        } else {
            mediaLibrary_->addItem(pathFromUtf8(path));
        }
    }
}

std::string MediaBinPanel::buildFileFilter() const {
    // Win32 filter: NUL-separated "description", "patterns" pairs, NUL-terminated.
    // Appended with explicit lengths — a "...\0" literal would stop at its first NUL.
    auto patternsFor = [](const std::vector<std::string>& exts, std::string patterns) {
        for (const auto& ext : exts) {
            if (!patterns.empty()) patterns += ";";
            patterns += "*" + ext;
        }
        return patterns;
    };
    std::string filter;
    auto addEntry = [&filter](const std::string& description, const std::string& patterns) {
        filter += description;
        filter.push_back('\0');
        filter += patterns;
        filter.push_back('\0');
    };

    const std::string images = patternsFor(MediaLibrary::getSupportedImageExtensions(), "");
    const std::string videos = patternsFor(MediaLibrary::getSupportedVideoExtensions(), "");
    addEntry("All Media Files", patternsFor(MediaLibrary::getSupportedVideoExtensions(), images));
    addEntry("Image Files", images);
    addEntry("Video Files", videos);
    addEntry("All Files", "*.*");
    filter.push_back('\0');
    return filter;
}

void MediaBinPanel::renderProceduralMenu() {
    if (ImGui::BeginPopup("ProceduralMenu")) {
        ImGui::TextDisabled("Create Procedural Source");
        ImGui::Separator();

        // Static patterns
        if (ImGui::BeginMenu("Patterns")) {
            if (ImGui::MenuItem("Solid Color")) {
                createProcedural(ProceduralType::SOLID_COLOR);
            }
            if (ImGui::MenuItem("Gradient")) {
                createProcedural(ProceduralType::GRADIENT);
            }
            if (ImGui::MenuItem("Checkerboard")) {
                createProcedural(ProceduralType::CHECKERBOARD);
            }
            if (ImGui::MenuItem("Grid")) {
                createProcedural(ProceduralType::GRID);
            }
            ImGui::EndMenu();
        }

        // Test patterns
        if (ImGui::BeginMenu("Test Patterns")) {
            if (ImGui::MenuItem("SMPTE Bars")) {
                createProcedural(ProceduralType::TEST_PATTERN);
            }
            ImGui::EndMenu();
        }

        // Animated patterns
        if (ImGui::BeginMenu("Animated")) {
            if (ImGui::MenuItem("Noise (Perlin/Simplex)")) {
                createProcedural(ProceduralType::NOISE);
            }
            if (ImGui::MenuItem("Plasma")) {
                createProcedural(ProceduralType::PLASMA);
            }
            ImGui::EndMenu();
        }

        ImGui::EndPopup();
    }
}

void MediaBinPanel::createProcedural(ProceduralType type) {
    if (!sourceManager_ || !mediaLibrary_) {
        return;
    }

    // Create the procedural source in SourceManager
    UUID sourceId = sourceManager_->createProceduralSource(type);
    if (sourceId.isNull()) {
        return;
    }

    // Create a MediaItem linked to this source
    auto item = std::make_unique<MediaItem>();
    item->setName(getProceduralTypeName(type));
    item->setType(MediaType::PROCEDURAL);
    item->setProceduralType(type);
    item->setSourceId(sourceId);

    // Add to media library
    UUID itemId = item->getId();
    mediaLibrary_->addItem(std::move(item));

    // Select the newly created item
    es_->selectedMediaId = itemId;

    // Optionally open editor for the new procedural
    if (proceduralEditCallback_) {
        proceduralEditCallback_(itemId);
    }
}

const char* MediaBinPanel::getMediaTypeIcon(MediaType type) const {
    switch (type) {
        case MediaType::IMAGE:
            return "[IMG]";
        case MediaType::VIDEO:
            return "[VID]";
        case MediaType::AUDIO:
            return "[AUD]";
        case MediaType::PROCEDURAL:
            return "[GEN]";
        default:
            return "[?]";
    }
}

} // namespace quadmapper
