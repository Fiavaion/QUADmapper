#pragma once

#include "Panel.hpp"

namespace quadmapper {

class SceneManager;

class ScenePanel : public Panel {
public:
    ScenePanel();

    // Set the scene manager to use (must be set before render())
    void setSceneManager(SceneManager* manager) { sceneManager_ = manager; }

    void render() override;

private:
    // Render the scene list with selection and actions
    void renderSceneList();

    // Render the action buttons (New, Duplicate, Delete)
    void renderActionButtons();

    // Handle drag-and-drop reordering
    void handleDragDrop(size_t sourceIndex);

    SceneManager* sceneManager_ = nullptr;

    // State for drag-and-drop reordering
    int dragSourceIndex_ = -1;

    // State for scene renaming
    bool isRenaming_ = false;
    size_t renamingIndex_ = 0;
    char renameBuffer_[128] = "";
};

} // namespace quadmapper
