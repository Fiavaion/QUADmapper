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

bool Scene::visibleObjectBounds(Vec3& min, Vec3& max) const {
    bool any = false;
    for (const auto& object : objects_) {
        if (!object->isVisible() || object->getMesh().empty()) continue;
        Vec3 lo, hi;
        object->worldBounds(lo, hi);
        min = any ? glm::min(min, lo) : lo;
        max = any ? glm::max(max, hi) : hi;
        any = true;
    }
    return any;
}

std::unique_ptr<Scene> Scene::duplicate(std::unordered_map<UUID, UUID>* quadIdMap) const {
    auto copy = std::make_unique<Scene>();
    copy->name_ = name_;
    copy->viewState_ = viewState_;
    copy->nextGroupNumber_ = nextGroupNumber_;

    std::unordered_map<UUID, UUID> quadIds;
    for (const auto& quad : quadManager_.getQuads()) {
        const UUID newId = UUID::generate();
        quadIds[quad->getId()] = newId;
        copy->quadManager_.insertQuad(std::make_unique<Quad>(quad->cloneWithId(newId)),
                                      copy->quadManager_.getQuadCount());
    }

    std::unordered_map<UUID, UUID> groupIds;
    for (const auto& group : groups_) groupIds[group->getId()] = UUID::generate();
    auto remapGroup = [&](const std::optional<UUID>& id) -> std::optional<UUID> {
        if (!id) return std::nullopt;
        auto it = groupIds.find(*id);
        return it != groupIds.end() ? std::optional<UUID>(it->second) : std::nullopt;
    };

    for (const auto& quad : quadManager_.getQuads()) {
        Quad* dup = copy->quadManager_.getQuad(quadIds[quad->getId()]);
        dup->setZOrder(quad->getZOrder());
        dup->setParentGroup(remapGroup(quad->getParentGroup()));
    }

    for (const auto& group : groups_) {
        auto dup = std::make_unique<QuadGroup>(groupIds[group->getId()]);
        dup->setName(group->getName());
        dup->setParentGroup(remapGroup(group->getParentGroup()));
        dup->setCollapsed(group->isCollapsed());
        dup->setVisible(group->isVisible());
        dup->setLocked(group->isLocked());
        for (const UUID& member : group->getMemberQuadIds()) {
            auto it = quadIds.find(member);
            if (it != quadIds.end()) dup->addMember(it->second);
        }
        copy->groups_.push_back(std::move(dup));
    }

    for (const auto& object : objects_) {
        copy->objects_.push_back(
            std::make_unique<MeshObject>(object->cloneWithId(UUID::generate())));
    }

    // The same quads selected, the primary one last so it stays primary.
    const std::optional<UUID> primary = quadManager_.getPrimarySelectedId();
    for (const UUID& id : quadManager_.getSelectedIds()) {
        if (!primary || id != *primary) copy->quadManager_.addToSelection(quadIds[id]);
    }
    if (primary && quadIds.count(*primary)) copy->quadManager_.addToSelection(quadIds[*primary]);

    if (quadIdMap) *quadIdMap = std::move(quadIds);
    return copy;
}

int findSceneIndex(const SceneList& scenes, const UUID& sceneId) {
    for (size_t i = 0; i < scenes.size(); ++i) {
        if (scenes[i] && scenes[i]->getId() == sceneId) return static_cast<int>(i);
    }
    return -1;
}

void assignSceneToOutput(SceneList& scenes, const UUID& sceneId, const UUID& outputId) {
    if (!outputId.isNull()) {
        for (auto& s : scenes) {
            if (s && s->getId() != sceneId && s->getAssignedOutputId() == outputId) {
                s->setAssignedOutputId(UUID::null());
            }
        }
    }
    for (auto& s : scenes) {
        if (s && s->getId() == sceneId) { s->setAssignedOutputId(outputId); break; }
    }
}

bool clearOutputAssignments(SceneList& scenes, const UUID& outputId) {
    if (outputId.isNull()) return false;
    bool cleared = false;
    for (auto& s : scenes) {
        if (s && s->getAssignedOutputId() == outputId) {
            s->setAssignedOutputId(UUID::null());
            cleared = true;
        }
    }
    return cleared;
}

std::string uniqueSceneCopyName(const SceneList& scenes, const std::string& baseName) {
    auto taken = [&](const std::string& name) {
        return std::any_of(scenes.begin(), scenes.end(),
                           [&](const auto& s) { return s && s->getName() == name; });
    };
    const std::string first = baseName + " copy";
    if (!taken(first)) return first;
    for (int n = 2; ; ++n) {
        std::string candidate = first + " " + std::to_string(n);
        if (!taken(candidate)) return candidate;
    }
}

bool moveScene(SceneList& scenes, size_t from, size_t to) {
    if (from >= scenes.size() || to >= scenes.size() || from == to) return false;
    auto moved = std::move(scenes[from]);
    scenes.erase(scenes.begin() + static_cast<std::ptrdiff_t>(from));
    scenes.insert(scenes.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    return true;
}

SceneTake planSceneTake(const SceneList& scenes, const UUID& activeSceneId, int number,
                        const std::vector<SceneTakeOutput>& outputs) {
    SceneTake take;
    if (number < 1 || number > static_cast<int>(scenes.size())) return take;
    const Scene* target = scenes[static_cast<size_t>(number - 1)].get();
    if (!target) return take;

    auto outputExists = [&](const UUID& id) {
        return !id.isNull() && std::any_of(outputs.begin(), outputs.end(),
                                           [&](const SceneTakeOutput& o) { return o.id == id; });
    };
    const bool targetIsActive = target->getId() == activeSceneId;
    take.sceneId = target->getId();

    if (outputExists(target->getAssignedOutputId())) {
        if (!targetIsActive) take.kind = SceneTake::Kind::Activate;
        return take;
    }

    UUID output;
    const int activeIndex = findSceneIndex(scenes, activeSceneId);
    if (activeIndex >= 0 && outputExists(scenes[static_cast<size_t>(activeIndex)]->getAssignedOutputId())) {
        output = scenes[static_cast<size_t>(activeIndex)]->getAssignedOutputId();
    } else {
        for (const SceneTakeOutput& o : outputs) {
            if (o.enabled && !o.id.isNull()) { output = o.id; break; }
        }
    }

    if (output.isNull()) {
        if (!targetIsActive) take.kind = SceneTake::Kind::Activate;
        return take;
    }
    take.kind = SceneTake::Kind::Route;
    take.outputId = output;
    return take;
}

} // namespace quadmapper
