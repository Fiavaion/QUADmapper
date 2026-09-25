#include "Scene.hpp"
#include <algorithm>
#include <sstream>

namespace quadmapper {

Scene::Scene() : id_(UUID::generate()), name_("New Scene") {}

Scene::Scene(const UUID& id) : id_(id), name_("New Scene") {}

void Scene::resetView() {
    viewState_ = SceneViewState();
}

Quad* Scene::createQuad() {
    return quadManager_.createQuad();
}

void Scene::deleteQuad(const UUID& id) {
    // Remove quad from all groups first
    for (auto& group : groups_) {
        group->removeMember(id);
    }
    quadManager_.deleteQuad(id);
}

Quad* Scene::getQuad(const UUID& id) {
    return quadManager_.getQuad(id);
}

// ============ Group Management Implementation ============

QuadGroup* Scene::createGroup(const std::string& name) {
    auto group = std::make_unique<QuadGroup>();
    group->setName(name.empty() ? generateUniqueGroupName() : name);
    QuadGroup* ptr = group.get();
    groups_.push_back(std::move(group));
    return ptr;
}

void Scene::deleteGroup(const UUID& id) {
    auto it = std::find_if(groups_.begin(), groups_.end(),
        [&id](const auto& g) { return g->getId() == id; });
    if (it != groups_.end()) {
        // Note: Deleting a group does NOT delete its member quads
        groups_.erase(it);
    }
}

void Scene::clearGroups() {
    groups_.clear();
    nextGroupNumber_ = 1;
}

QuadGroup* Scene::getGroup(const UUID& id) {
    auto it = std::find_if(groups_.begin(), groups_.end(),
        [&id](const auto& g) { return g->getId() == id; });
    return it != groups_.end() ? it->get() : nullptr;
}

const QuadGroup* Scene::getGroup(const UUID& id) const {
    auto it = std::find_if(groups_.begin(), groups_.end(),
        [&id](const auto& g) { return g->getId() == id; });
    return it != groups_.end() ? it->get() : nullptr;
}

void Scene::addQuadToGroup(const UUID& groupId, const UUID& quadId) {
    QuadGroup* group = getGroup(groupId);
    if (group && quadManager_.getQuad(quadId)) {
        group->addMember(quadId);
    }
}

void Scene::removeQuadFromGroup(const UUID& groupId, const UUID& quadId) {
    QuadGroup* group = getGroup(groupId);
    if (group) {
        group->removeMember(quadId);
    }
}

std::vector<QuadGroup*> Scene::getGroupsForQuad(const UUID& quadId) {
    std::vector<QuadGroup*> result;
    for (auto& group : groups_) {
        if (group->hasMember(quadId)) {
            result.push_back(group.get());
        }
    }
    return result;
}

std::vector<const QuadGroup*> Scene::getGroupsForQuad(const UUID& quadId) const {
    std::vector<const QuadGroup*> result;
    for (const auto& group : groups_) {
        if (group->hasMember(quadId)) {
            result.push_back(group.get());
        }
    }
    return result;
}

bool Scene::isQuadInGroup(const UUID& groupId, const UUID& quadId) const {
    const QuadGroup* group = getGroup(groupId);
    return group && group->hasMember(quadId);
}

QuadGroup* Scene::createGroupFromSelection(const std::string& name) {
    if (!quadManager_.hasSelection()) {
        return nullptr;
    }

    std::string groupName = name.empty() ? generateUniqueGroupName() : name;
    QuadGroup* group = createGroup(groupName);

    // Add all selected quads to the group
    for (const UUID& quadId : quadManager_.getSelectedIds()) {
        group->addMember(quadId);
    }

    return group;
}

void Scene::applyGroupVisibility(const UUID& groupId, bool visible) {
    QuadGroup* group = getGroup(groupId);
    if (!group) return;

    group->setVisible(visible);

    // Apply to all member quads
    for (const UUID& quadId : group->getMemberQuadIds()) {
        Quad* quad = quadManager_.getQuad(quadId);
        if (quad) {
            quad->setVisible(visible);
        }
    }
}

void Scene::applyGroupLock(const UUID& groupId, bool locked) {
    QuadGroup* group = getGroup(groupId);
    if (!group) return;

    group->setLocked(locked);

    // Apply to all member quads
    for (const UUID& quadId : group->getMemberQuadIds()) {
        Quad* quad = quadManager_.getQuad(quadId);
        if (quad) {
            quad->setLocked(locked);
        }
    }
}

std::string Scene::generateUniqueGroupName() const {
    int number = nextGroupNumber_;
    std::string baseName = "Group ";

    // Find a unique name
    bool found = true;
    while (found) {
        found = false;
        std::ostringstream oss;
        oss << baseName << number;
        std::string candidateName = oss.str();

        for (const auto& group : groups_) {
            if (group->getName() == candidateName) {
                found = true;
                number++;
                break;
            }
        }

        if (!found) {
            // Update the next number for next time
            const_cast<Scene*>(this)->nextGroupNumber_ = number + 1;
            return candidateName;
        }
    }

    // Fallback
    std::ostringstream oss;
    oss << baseName << number;
    return oss.str();
}

MeshObject* Scene::createObject(const std::string& name) {
    auto object = std::make_unique<MeshObject>();
    object->setName(name);
    return adoptObject(std::move(object));
}

MeshObject* Scene::adoptObject(std::unique_ptr<MeshObject> object) {
    if (!object) return nullptr;
    objects_.push_back(std::move(object));
    return objects_.back().get();
}

void Scene::deleteObject(const UUID& id) {
    objects_.erase(std::remove_if(objects_.begin(), objects_.end(),
                                  [&](const auto& o) { return o->getId() == id; }),
                   objects_.end());
}

MeshObject* Scene::getObject(const UUID& id) {
    for (const auto& object : objects_) {
        if (object->getId() == id) return object.get();
    }
    return nullptr;
}

} // namespace quadmapper
