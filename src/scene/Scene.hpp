#pragma once

#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "Quad.hpp"
#include "QuadGroup.hpp"
#include "QuadManager.hpp"
#include <string>
#include <vector>
#include <memory>

namespace quadmapper {

// View state for a scene (pan, zoom, grid settings)
struct SceneViewState {
    Vec2 panOffset = Vec2(0.0f, 0.0f);
    float zoomLevel = 1.0f;
    bool gridVisible = true;
    bool snapToGrid = false;
};

class Scene {
public:
    Scene();
    explicit Scene(const UUID& id);

    const UUID& getId() const { return id_; }
    const std::string& getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    bool isActive() const { return isActive_; }
    void setActive(bool active) { isActive_ = active; }

    // View state access
    SceneViewState& getViewState() { return viewState_; }
    const SceneViewState& getViewState() const { return viewState_; }
    void resetView();

    // QuadManager access
    QuadManager& getQuadManager() { return quadManager_; }
    const QuadManager& getQuadManager() const { return quadManager_; }

    // Convenience methods that delegate to QuadManager
    Quad* createQuad();
    void deleteQuad(const UUID& id);
    Quad* getQuad(const UUID& id);
    const std::vector<std::unique_ptr<Quad>>& getQuads() const { return quadManager_.getQuads(); }
    std::vector<Quad*> getQuadsByZOrder() const { return quadManager_.getQuadsByZOrder(); }

    // ============ Group Management API ============

    // Group creation and deletion
    QuadGroup* createGroup(const std::string& name = "New Group");
    void deleteGroup(const UUID& id);
    void clearGroups();

    // Group access
    QuadGroup* getGroup(const UUID& id);
    const QuadGroup* getGroup(const UUID& id) const;
    const std::vector<std::unique_ptr<QuadGroup>>& getGroups() const { return groups_; }
    size_t getGroupCount() const { return groups_.size(); }

    // Group membership management
    void addQuadToGroup(const UUID& groupId, const UUID& quadId);
    void removeQuadFromGroup(const UUID& groupId, const UUID& quadId);
    std::vector<QuadGroup*> getGroupsForQuad(const UUID& quadId);
    std::vector<const QuadGroup*> getGroupsForQuad(const UUID& quadId) const;
    bool isQuadInGroup(const UUID& groupId, const UUID& quadId) const;

    // Create group from current selection in QuadManager
    QuadGroup* createGroupFromSelection(const std::string& name = "New Group");

    // Apply group visibility/lock to all member quads
    void applyGroupVisibility(const UUID& groupId, bool visible);
    void applyGroupLock(const UUID& groupId, bool locked);

    // Generate unique group name
    std::string generateUniqueGroupName() const;

private:
    UUID id_;
    std::string name_;
    bool isActive_ = false;
    SceneViewState viewState_;
    QuadManager quadManager_;
    std::vector<std::unique_ptr<QuadGroup>> groups_;
    int nextGroupNumber_ = 1;
};

} // namespace quadmapper
