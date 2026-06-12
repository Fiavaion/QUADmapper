#pragma once

#include "Scene.hpp"
#include <vector>
#include <memory>
#include <optional>

namespace quadmapper {

class SceneManager {
public:
    SceneManager();
    ~SceneManager();

    Scene* createScene(const std::string& name = "New Scene");
    void deleteScene(const UUID& id);
    Scene* getScene(const UUID& id);
    Scene* getSceneByIndex(size_t index);
    const Scene* getSceneByIndex(size_t index) const;
    Scene* getActiveScene();
    const Scene* getActiveScene() const;
    void setActiveScene(const UUID& id);

    // Set active scene by index (for keyboard shortcuts 1-9)
    void setActiveSceneByIndex(size_t index);

    size_t getSceneCount() const { return scenes_.size(); }
    const std::vector<std::unique_ptr<Scene>>& getScenes() const { return scenes_; }

    // Check if scene can be deleted (must have more than one scene)
    bool canDeleteScene() const { return scenes_.size() > 1; }

    // Get index of a scene by ID (returns nullopt if not found)
    std::optional<size_t> getSceneIndex(const UUID& id) const;

    // Get the ID of the active scene
    const UUID& getActiveSceneId() const { return activeSceneId_; }

    // Duplicate scene with a new unique ID and name
    Scene* duplicateScene(const UUID& id);

    // Generate unique scene name (e.g., "Scene 2", "Scene 3", etc.)
    std::string generateUniqueSceneName() const;

    // Reorder scenes: move scene at fromIndex to toIndex
    void moveScene(size_t fromIndex, size_t toIndex);

    // Reset to default state (clears all scenes and creates one default)
    void reset();

private:
    std::vector<std::unique_ptr<Scene>> scenes_;
    UUID activeSceneId_;
};

} // namespace quadmapper
