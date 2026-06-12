#include "MediaBinPanel.hpp"
#include "media/MediaLibrary.hpp"
#include "media/MediaItem.hpp"
#include "media/ThumbnailCache.hpp"
#include "source/SourceManager.hpp"
#include "source/procedural/ProceduralFactory.hpp"
#include <imgui.h>
#include <algorithm>
#include <glad/glad.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
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

MediaBinPanel::MediaBinPanel() : Panel("Media Bin") {
    selectedItemId_ = UUID::null();
    contextMenuItemId_ = UUID::null();
}

void MediaBinPanel::render() {
    if (!visible_) return;

    ImGui::Begin(title_.c_str(), &visible_);
    renderContent();
    ImGui::End();
}

void MediaBinPanel::renderContent() {
    if (!mediaLibrary_) {
        ImGui::TextDisabled("No media library connected");
        return;
    }

    renderControls();
    ImGui::Separator();
    renderThumbnailGrid();
}

void MediaBinPanel::renderControls() {
    // Import button
    if (ImGui::Button("Import...")) {
        handleImport();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Import media files (images, videos)");
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

    ImGui::SameLine();

    // Thumbnail size slider
    ImGui::SetNextItemWidth(100.0f);
    ImGui::SliderFloat("##size", &thumbnailSize_, 48.0f, 128.0f, "%.0f px");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Thumbnail size");
    }

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
        ImGui::TextDisabled("Click 'Import...' to add media.");
        ImGui::TextDisabled("Drag files here to import.");
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
}

void MediaBinPanel::renderMediaItem(MediaItem* item, int index) {
    ImGui::PushID(index);

    bool isSelected = (item->getId() == selectedItemId_);
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
        selectedItemId_ = item->getId();
    }

    // Handle right-click for context menu
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        selectedItemId_ = item->getId();
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

    // Try to get a real thumbnail for image items
    if (thumbnailCache_ && item->getType() == MediaType::IMAGE) {
        GLuint thumbTex = thumbnailCache_->getThumbnail(item->getPath(),
                                                         static_cast<int>(thumbnailSize_));
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
            ImGui::TextDisabled("Path: %s", item->getPath().string().c_str());
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
            if (selectedItemId_ == item->getId()) {
                selectedItemId_ = UUID::null();
            }
            // Also remove the source from SourceManager if it exists
            if (sourceManager_ && item->hasSource()) {
                sourceManager_->removeSource(item->getSourceId());
            }
            mediaLibrary_->removeItem(item->getId());
        }

#ifdef _WIN32
        // Only show file-related options for non-procedural items
        if (item->getType() != MediaType::PROCEDURAL) {
            if (ImGui::MenuItem("Reveal in Explorer")) {
                std::wstring path = item->getPath().wstring();
                // Use ShellExecute to open explorer and select the file
                std::wstring param = L"/select,\"" + path + L"\"";
                ShellExecuteW(NULL, L"open", L"explorer.exe", param.c_str(), NULL, SW_SHOWNORMAL);
            }

            if (ImGui::MenuItem("Copy Path")) {
                ImGui::SetClipboardText(item->getPath().string().c_str());
            }
        }
#endif

        ImGui::EndPopup();
    }
}

void MediaBinPanel::handleImport() {
#ifdef _WIN32
    // Build file filter
    std::string filter = buildFileFilter();

    // Setup open file dialog
    char filename[MAX_PATH * 10] = "";  // Support multiple files

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = filename;
    ofn.nMaxFile = sizeof(filename);
    ofn.lpstrTitle = "Import Media Files";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (GetOpenFileNameA(&ofn)) {
        // Parse selected files
        char* ptr = filename;
        std::string directory = ptr;
        ptr += directory.length() + 1;

        if (*ptr == '\0') {
            // Single file selected
            mediaLibrary_->addItem(std::filesystem::path(directory));
        } else {
            // Multiple files selected - directory followed by filenames
            while (*ptr != '\0') {
                std::string file = ptr;
                std::filesystem::path fullPath = std::filesystem::path(directory) / file;
                mediaLibrary_->addItem(fullPath);
                ptr += file.length() + 1;
            }
        }
    }
#else
    // On non-Windows platforms, show a message
    // A cross-platform file dialog would require additional library (e.g., nfd)
    ImGui::OpenPopup("ImportNotSupported");
#endif
}

std::string MediaBinPanel::buildFileFilter() const {
    std::string filter;

    // All supported files
    filter += "All Media Files\0";
    std::string allExts;
    for (const auto& ext : MediaLibrary::getSupportedImageExtensions()) {
        if (!allExts.empty()) allExts += ";";
        allExts += "*" + ext;
    }
    for (const auto& ext : MediaLibrary::getSupportedVideoExtensions()) {
        if (!allExts.empty()) allExts += ";";
        allExts += "*" + ext;
    }
    filter += allExts + '\0';

    // Image files
    filter += "Image Files\0";
    std::string imgExts;
    for (const auto& ext : MediaLibrary::getSupportedImageExtensions()) {
        if (!imgExts.empty()) imgExts += ";";
        imgExts += "*" + ext;
    }
    filter += imgExts + '\0';

    // Video files
    filter += "Video Files\0";
    std::string vidExts;
    for (const auto& ext : MediaLibrary::getSupportedVideoExtensions()) {
        if (!vidExts.empty()) vidExts += ";";
        vidExts += "*" + ext;
    }
    filter += vidExts + '\0';

    // All files
    filter += "All Files\0*.*\0";

    // Double null terminator
    filter += '\0';

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
    selectedItemId_ = itemId;

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
