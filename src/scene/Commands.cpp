#include "Commands.hpp"
#include "Quad.hpp"
#include "QuadGroup.hpp"
#include "QuadManager.hpp"
#include "Scene.hpp"
#include "core/Logger.hpp"
#include <algorithm>

namespace quadmapper {

// SceneListResolver / QuadRef
Scene* SceneListResolver::findScene(const UUID& sceneId) {
    for (auto& scene : scenes_) {
        if (scene && scene->getId() == sceneId) return scene.get();
    }
    return nullptr;
}

Scene* QuadRef::scene() const {
    return resolver ? resolver->findScene(sceneId) : nullptr;
}

Quad* QuadRef::quad() const {
    Scene* s = scene();
    return s ? s->getQuad(quadId) : nullptr;
}

Quad* resolveForCommand(const QuadRef& ref, const std::string& commandDescription) {
    Quad* quad = ref.quad();
    if (!quad) {
        LOG_WARNING("'{}': quad {} no longer exists; skipped", commandDescription,
                    ref.quadId.toString());
    }
    return quad;
}

// Restore a full-state snapshot (same id) into the scene at its saved z-order.
static Quad* restoreSnapshot(Scene& scene, const Quad& snapshot) {
    return scene.getQuadManager().insertQuad(
        std::make_unique<Quad>(snapshot), static_cast<size_t>(std::max(0, snapshot.getZOrder())));
}

// MoveCornerCommand implementation
MoveCornerCommand::MoveCornerCommand(const QuadRef& target, int cornerIndex, const Vec2& oldPosition, const Vec2& newPosition)
    : target_(target)
    , cornerIndex_(cornerIndex)
    , oldPosition_(oldPosition)
    , newPosition_(newPosition)
{
}

bool MoveCornerCommand::execute() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->getGeometry().setCorner(cornerIndex_, newPosition_);
    return true;
}

bool MoveCornerCommand::undo() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->getGeometry().setCorner(cornerIndex_, oldPosition_);
    return true;
}

std::string MoveCornerCommand::getDescription() const {
    static const char* cornerNames[] = {"top-left", "top-right", "bottom-right", "bottom-left"};
    return std::string("Move ") + cornerNames[cornerIndex_] + " corner";
}

bool MoveCornerCommand::canMergeWith(const Command& other) const {
    auto* otherMove = dynamic_cast<const MoveCornerCommand*>(&other);
    if (!otherMove) return false;
    return target_.sameTarget(otherMove->target_) && cornerIndex_ == otherMove->cornerIndex_;
}

void MoveCornerCommand::mergeWith(const Command& other) {
    auto* otherMove = dynamic_cast<const MoveCornerCommand*>(&other);
    if (otherMove) {
        newPosition_ = otherMove->newPosition_;
    }
}

// MoveMeshVertexCommand implementation
MoveMeshVertexCommand::MoveMeshVertexCommand(const QuadRef& target, int vertexIndex, const Vec2& oldOffset, const Vec2& newOffset)
    : target_(target)
    , vertexIndex_(vertexIndex)
    , oldOffset_(oldOffset)
    , newOffset_(newOffset)
{
}

bool MoveMeshVertexCommand::execute() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->getGeometry().setMeshVertexByIndex(vertexIndex_, newOffset_);
    return true;
}

bool MoveMeshVertexCommand::undo() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->getGeometry().setMeshVertexByIndex(vertexIndex_, oldOffset_);
    return true;
}

std::string MoveMeshVertexCommand::getDescription() const {
    return "Move mesh vertex";
}

bool MoveMeshVertexCommand::canMergeWith(const Command& other) const {
    auto* otherMove = dynamic_cast<const MoveMeshVertexCommand*>(&other);
    if (!otherMove) return false;
    return target_.sameTarget(otherMove->target_) && vertexIndex_ == otherMove->vertexIndex_;
}

void MoveMeshVertexCommand::mergeWith(const Command& other) {
    auto* otherMove = dynamic_cast<const MoveMeshVertexCommand*>(&other);
    if (otherMove) {
        newOffset_ = otherMove->newOffset_;
    }
}

// CreateQuadCommand implementation
CreateQuadCommand::CreateQuadCommand(QuadResolver& resolver, const UUID& sceneId, const std::string& name)
    : created_{&resolver, sceneId, UUID()}
    , name_(name)
{
}

bool CreateQuadCommand::execute() {
    Scene* scene = created_.scene();
    if (!scene) {
        LOG_WARNING("'{}': scene no longer exists; skipped", getDescription());
        return false;
    }

    if (snapshot_) {
        // Redo - restore the original quad (same id, same state)
        return restoreSnapshot(*scene, *snapshot_) != nullptr;
    }

    QuadManager& manager = scene->getQuadManager();
    Quad* quad = name_.empty() ? manager.createQuad() : manager.createQuad(name_);
    if (!quad) return false;
    created_.quadId = quad->getId();
    return true;
}

bool CreateQuadCommand::undo() {
    Quad* quad = resolveForCommand(created_, getDescription());
    if (!quad) return false;
    snapshot_ = *quad;
    created_.scene()->deleteQuad(created_.quadId);
    return true;
}

std::string CreateQuadCommand::getDescription() const {
    return "Create quad";
}

// DeleteQuadCommand implementation
DeleteQuadCommand::DeleteQuadCommand(const QuadRef& target)
    : target_(target)
{
}

bool DeleteQuadCommand::execute() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;

    // Snapshot at execute time (not construction) so the saved z-order is the
    // quad's position right before this delete; compound undo runs in reverse.
    Scene* scene = target_.scene();
    snapshot_ = *quad;
    groupIds_.clear();
    for (const QuadGroup* group : scene->getGroupsForQuad(target_.quadId)) {
        groupIds_.push_back(group->getId());
    }
    scene->deleteQuad(target_.quadId);
    return true;
}

bool DeleteQuadCommand::undo() {
    if (!snapshot_) return false;
    Scene* scene = target_.scene();
    if (!scene) {
        LOG_WARNING("'{}': scene no longer exists; skipped", getDescription());
        return false;
    }
    Quad* restored = restoreSnapshot(*scene, *snapshot_);
    if (!restored) return false;

    for (const UUID& groupId : groupIds_) {
        scene->addQuadToGroup(groupId, target_.quadId);  // skips groups deleted since
    }
    scene->getQuadManager().addToSelection(target_.quadId);
    return true;
}

std::string DeleteQuadCommand::getDescription() const {
    return "Delete quad";
}

// DuplicateQuadCommand implementation
DuplicateQuadCommand::DuplicateQuadCommand(const QuadRef& source)
    : source_(source)
    , duplicate_{source.resolver, source.sceneId, UUID::generate()}
{
}

bool DuplicateQuadCommand::execute() {
    Scene* scene = source_.scene();
    if (!scene) {
        LOG_WARNING("'{}': scene no longer exists; skipped", getDescription());
        return false;
    }

    Quad* dup = nullptr;
    if (snapshot_) {
        // Redo - restore the same duplicate (same id, same state)
        dup = restoreSnapshot(*scene, *snapshot_);
    } else {
        if (!resolveForCommand(source_, getDescription())) return false;
        dup = scene->getQuadManager().duplicateQuad(source_.quadId, duplicate_.quadId);
    }
    if (!dup) return false;

    scene->getQuadManager().selectQuad(dup->getId());
    return true;
}

bool DuplicateQuadCommand::undo() {
    Quad* dup = resolveForCommand(duplicate_, getDescription());
    if (!dup) return false;
    snapshot_ = *dup;
    Scene* scene = duplicate_.scene();
    scene->deleteQuad(duplicate_.quadId);
    scene->getQuadManager().selectQuad(source_.quadId);  // no-op if the source is gone
    return true;
}

std::string DuplicateQuadCommand::getDescription() const {
    return "Duplicate quad";
}

// SetQuadVisibilityCommand implementation
SetQuadVisibilityCommand::SetQuadVisibilityCommand(const QuadRef& target, bool newVisible)
    : target_(target)
    , oldVisible_(true)
    , newVisible_(newVisible)
{
    if (const Quad* quad = target_.quad()) oldVisible_ = quad->isVisible();
}

bool SetQuadVisibilityCommand::execute() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->setVisible(newVisible_);
    return true;
}

bool SetQuadVisibilityCommand::undo() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->setVisible(oldVisible_);
    return true;
}

std::string SetQuadVisibilityCommand::getDescription() const {
    return newVisible_ ? "Show quad" : "Hide quad";
}

// SetQuadLockedCommand implementation
SetQuadLockedCommand::SetQuadLockedCommand(const QuadRef& target, bool newLocked)
    : target_(target)
    , oldLocked_(false)
    , newLocked_(newLocked)
{
    if (const Quad* quad = target_.quad()) oldLocked_ = quad->isLocked();
}

bool SetQuadLockedCommand::execute() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->setLocked(newLocked_);
    return true;
}

bool SetQuadLockedCommand::undo() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->setLocked(oldLocked_);
    return true;
}

std::string SetQuadLockedCommand::getDescription() const {
    return newLocked_ ? "Lock quad" : "Unlock quad";
}

// SetQuadOpacityCommand implementation
SetQuadOpacityCommand::SetQuadOpacityCommand(const QuadRef& target, float newOpacity)
    : target_(target)
    , oldOpacity_(1.0f)
    , newOpacity_(newOpacity)
{
    if (const Quad* quad = target_.quad()) oldOpacity_ = quad->getOpacity();
}

bool SetQuadOpacityCommand::execute() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->setOpacity(newOpacity_);
    return true;
}

bool SetQuadOpacityCommand::undo() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->setOpacity(oldOpacity_);
    return true;
}

std::string SetQuadOpacityCommand::getDescription() const {
    return "Change opacity";
}

bool SetQuadOpacityCommand::canMergeWith(const Command& other) const {
    auto* otherCmd = dynamic_cast<const SetQuadOpacityCommand*>(&other);
    return otherCmd && target_.sameTarget(otherCmd->target_);
}

void SetQuadOpacityCommand::mergeWith(const Command& other) {
    auto* otherCmd = dynamic_cast<const SetQuadOpacityCommand*>(&other);
    if (otherCmd) {
        newOpacity_ = otherCmd->newOpacity_;
    }
}

// RenameQuadCommand implementation
RenameQuadCommand::RenameQuadCommand(const QuadRef& target, const std::string& newName)
    : target_(target)
    , newName_(newName)
{
    if (const Quad* quad = target_.quad()) oldName_ = quad->getName();
}

bool RenameQuadCommand::execute() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->setName(newName_);
    return true;
}

bool RenameQuadCommand::undo() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->setName(oldName_);
    return true;
}

std::string RenameQuadCommand::getDescription() const {
    return "Rename quad";
}

// SetMeshResolutionCommand implementation
SetMeshResolutionCommand::SetMeshResolutionCommand(const QuadRef& target, int newResolution)
    : target_(target)
    , oldResolution_(16)
    , newResolution_(newResolution)
{
    if (const Quad* quad = target_.quad()) {
        oldResolution_ = quad->getGeometry().getMeshResolution();
        oldMeshVertices_ = quad->getGeometry().getMeshVertices();
    }
}

bool SetMeshResolutionCommand::execute() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->getGeometry().setMeshResolution(newResolution_);
    return true;
}

bool SetMeshResolutionCommand::undo() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->getGeometry().setMeshResolution(oldResolution_);
    quad->getGeometry().setMeshVertices(oldMeshVertices_);
    return true;
}

std::string SetMeshResolutionCommand::getDescription() const {
    return "Change mesh resolution";
}

// MoveQuadCommand implementation
MoveQuadCommand::MoveQuadCommand(const QuadRef& target, const Vec2& delta)
    : target_(target)
    , oldCorners_{}
    , totalDelta_(delta)
{
    if (const Quad* quad = target_.quad()) {
        oldCorners_ = quad->getGeometry().getCorners();
    }
}

bool MoveQuadCommand::execute() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    auto corners = oldCorners_;
    for (auto& corner : corners) {
        corner += totalDelta_;
    }
    quad->getGeometry().setCorners(corners);
    return true;
}

bool MoveQuadCommand::undo() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->getGeometry().setCorners(oldCorners_);
    return true;
}

std::string MoveQuadCommand::getDescription() const {
    return "Move quad";
}

bool MoveQuadCommand::canMergeWith(const Command& other) const {
    auto* otherMove = dynamic_cast<const MoveQuadCommand*>(&other);
    return otherMove && target_.sameTarget(otherMove->target_);
}

void MoveQuadCommand::mergeWith(const Command& other) {
    auto* otherMove = dynamic_cast<const MoveQuadCommand*>(&other);
    if (otherMove) {
        totalDelta_ += otherMove->totalDelta_;
    }
}

} // namespace quadmapper
