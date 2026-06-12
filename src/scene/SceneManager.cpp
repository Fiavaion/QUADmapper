#include "SceneManager.hpp"
#include "core/Logger.hpp"
#include <algorithm>
#include <sstream>

namespace quadmapper {

SceneManager::SceneManager() {
    // Create default scene
    createScene("Scene 1");
}

SceneManager::~SceneManager() = default;

Scene* SceneManager::createScene(const std::string& name) {
    auto scene = std::make_unique<Scene>();
    scene->setName(name);
    Scene* ptr = scene.get();

    // If this is the first scene, make it active
    if (scenes_.empty()) {
        scene->setActive(true);
        activeSceneId_ = scene->getId();
    }

    scenes_.push_back(std::move(scene));
    return ptr;
}

void SceneManager::deleteScene(const UUID& id) {
    // Don't delete the last scene
    if (scenes_.size() <= 1) return;

    auto it = std::find_if(scenes_.begin(), scenes_.end(),
        [&id](const auto& s) { return s->getId() == id; });

    if (it != scenes_.end()) {
        bool wasActive = (*it)->isActive();
        scenes_.erase(it);

        // If deleted scene was active, activate another
        if (wasActive && !scenes_.empty()) {
            scenes_[0]->setActive(true);
            activeSceneId_ = scenes_[0]->getId();
        }
    }
}

Scene* SceneManager::getScene(const UUID& id) {
    auto it = std::find_if(scenes_.begin(), scenes_.end(),
        [&id](const auto& s) { return s->getId() == id; });
    return it != scenes_.end() ? it->get() : nullptr;
}

Scene* SceneManager::getActiveScene() {
    return getScene(activeSceneId_);
}

const Scene* SceneManager::getActiveScene() const {
    auto it = std::find_if(scenes_.begin(), scenes_.end(),
        [this](const auto& s) { return s->getId() == activeSceneId_; });
    return it != scenes_.end() ? it->get() : nullptr;
}

void SceneManager::setActiveScene(const UUID& id) {
    Scene* newActive = getScene(id);
    if (!newActive) return;

    // Deactivate current
    if (Scene* current = getActiveScene()) {
        current->setActive(false);
    }

    // Activate new
    newActive->setActive(true);
    activeSceneId_ = id;
}

void SceneManager::setActiveSceneByIndex(size_t index) {
    if (index >= scenes_.size()) return;
    setActiveScene(scenes_[index]->getId());
}

std::optional<size_t> SceneManager::getSceneIndex(const UUID& id) const {
    for (size_t i = 0; i < scenes_.size(); ++i) {
        if (scenes_[i]->getId() == id) {
            return i;
        }
    }
    return std::nullopt;
}

void SceneManager::moveScene(size_t fromIndex, size_t toIndex) {
    if (fromIndex >= scenes_.size() || toIndex >= scenes_.size()) {
        return;
    }
    if (fromIndex == toIndex) {
        return;
    }

    // Extract the scene from its current position
    auto scene = std::move(scenes_[fromIndex]);
    scenes_.erase(scenes_.begin() + static_cast<std::ptrdiff_t>(fromIndex));

    // Adjust toIndex if needed (after removal)
    if (toIndex > fromIndex) {
        // No adjustment needed, the removal shifted elements down
    }

    // Insert at new position
    scenes_.insert(scenes_.begin() + static_cast<std::ptrdiff_t>(toIndex), std::move(scene));

    Logger::debug("Moved scene from index {} to {}", fromIndex, toIndex);
}

Scene* SceneManager::getSceneByIndex(size_t index) {
    if (index >= scenes_.size()) {
        return nullptr;
    }
    return scenes_[index].get();
}

const Scene* SceneManager::getSceneByIndex(size_t index) const {
    if (index >= scenes_.size()) {
        return nullptr;
    }
    return scenes_[index].get();
}

Scene* SceneManager::duplicateScene(const UUID& id) {
    const Scene* source = getScene(id);
    if (!source) {
        Logger::warning("Cannot duplicate scene: source scene not found");
        return nullptr;
    }

    // Create new scene with unique name
    std::string newName = source->getName() + " Copy";
    auto newScene = std::make_unique<Scene>();
    newScene->setName(newName);

    // Copy view state
    newScene->getViewState() = source->getViewState();

    // Deep copy all quads from source scene
    const auto& sourceQuads = source->getQuads();
    for (const auto& srcQuadPtr : sourceQuads) {
        const Quad& srcQuad = *srcQuadPtr;

        // Create new quad in destination scene
        Quad* newQuad = newScene->createQuad();

        // Copy properties
        newQuad->setName(srcQuad.getName());
        newQuad->setZOrder(srcQuad.getZOrder());
        newQuad->setVisible(srcQuad.isVisible());
        newQuad->setLocked(srcQuad.isLocked());
        newQuad->setOpacity(srcQuad.getOpacity());

        // Copy geometry
        auto& newGeometry = newQuad->getGeometry();
        const auto& srcGeometry = srcQuad.getGeometry();
        newGeometry.setCorners(srcGeometry.getCorners());
        newGeometry.setMeshResolution(srcGeometry.getMeshResolution());
        newGeometry.setMeshMode(srcGeometry.getMeshMode());
        if (srcGeometry.hasMeshModifications()) {
            newGeometry.setMeshVertices(srcGeometry.getMeshVertices());
        }

        // Copy texture binding
        newQuad->getTexture() = srcQuad.getTexture();
    }

    // Deep copy all groups from source scene
    // Note: Group member IDs won't match since quads have new UUIDs.
    // For a full implementation, we'd need to map old quad IDs to new ones.
    // For now, groups are not copied as quad IDs are regenerated.

    Scene* ptr = newScene.get();
    scenes_.push_back(std::move(newScene));

    Logger::debug("Duplicated scene '{}' to '{}'", source->getName(), ptr->getName());
    return ptr;
}

std::string SceneManager::generateUniqueSceneName() const {
    int number = static_cast<int>(scenes_.size()) + 1;
    std::string name;

    // Find a unique name
    bool unique = false;
    while (!unique) {
        std::ostringstream oss;
        oss << "Scene " << number;
        name = oss.str();

        unique = true;
        for (const auto& scene : scenes_) {
            if (scene->getName() == name) {
                unique = false;
                ++number;
                break;
            }
        }
    }

    return name;
}

void SceneManager::reset() {
    scenes_.clear();
    activeSceneId_ = UUID();

    // Create one default scene
    createScene("Scene 1");

    Logger::debug("SceneManager reset to default state");
}

} // namespace quadmapper
