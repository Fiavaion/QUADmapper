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

// MoveGridHandleCommand implementation
MoveGridHandleCommand::MoveGridHandleCommand(const QuadRef& target, int vertexIndex, int handle,
                                             const VertexTangents& oldTangents,
                                             const VertexTangents& newTangents)
    : target_(target)
    , vertexIndex_(vertexIndex)
    , handle_(handle)
    , oldTangents_(oldTangents)
    , newTangents_(newTangents)
{
}

bool MoveGridHandleCommand::execute() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->getGeometry().setVertexTangent(vertexIndex_, newTangents_);
    return true;
}

bool MoveGridHandleCommand::undo() {
    Quad* quad = resolveForCommand(target_, getDescription());
    if (!quad) return false;
    quad->getGeometry().setVertexTangent(vertexIndex_, oldTangents_);
    return true;
}

std::string MoveGridHandleCommand::getDescription() const {
    return "Move grid handle";
}

bool MoveGridHandleCommand::canMergeWith(const Command& other) const {
    auto* otherMove = dynamic_cast<const MoveGridHandleCommand*>(&other);
    if (!otherMove) return false;
    return target_.sameTarget(otherMove->target_) && vertexIndex_ == otherMove->vertexIndex_ &&
           handle_ == otherMove->handle_;
}

void MoveGridHandleCommand::mergeWith(const Command& other) {
    auto* otherMove = dynamic_cast<const MoveGridHandleCommand*>(&other);
    if (otherMove) {
        newTangents_ = otherMove->newTangents_;
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

namespace {

// The media a snapshot quad would bring back, if any.
void collectSnapshotMedia(const std::optional<Quad>& snapshot, std::unordered_set<UUID>& out) {
    if (snapshot && !snapshot->getTexture().mediaId.isNull()) {
        out.insert(snapshot->getTexture().mediaId);
    }
}

}  // namespace

void CreateQuadCommand::collectMediaIds(std::unordered_set<UUID>& out) const {
    collectSnapshotMedia(snapshot_, out);
}

void DeleteQuadCommand::collectMediaIds(std::unordered_set<UUID>& out) const {
    collectSnapshotMedia(snapshot_, out);
}

void DuplicateQuadCommand::collectMediaIds(std::unordered_set<UUID>& out) const {
    collectSnapshotMedia(snapshot_, out);
}

// ---- 3D objects ------------------------------------------------------------------

Scene* ObjectRef::scene() const {
    return resolver ? resolver->findScene(sceneId) : nullptr;
}

MeshObject* ObjectRef::object() const {
    Scene* s = scene();
    return s ? s->getObject(objectId) : nullptr;
}

MeshObject* resolveObjectForCommand(const ObjectRef& ref, const std::string& commandDescription) {
    MeshObject* object = ref.object();
    if (!object) {
        LOG_WARNING("'{}': object {} no longer exists; skipped", commandDescription,
                    ref.objectId.toString());
    }
    return object;
}

namespace {

void collectHeldMedia(const std::unique_ptr<MeshObject>& held, std::unordered_set<UUID>& out) {
    if (held && !held->getTexture().mediaId.isNull()) out.insert(held->getTexture().mediaId);
}

void remapHeldMedia(const std::unique_ptr<MeshObject>& held, const UUID& from, const UUID& to) {
    if (held && held->getTexture().mediaId == from) held->getTexture().mediaId = to;
}

// Put a held object back into its scene; false (and the object stays held) when the
// scene is gone or already has an object with that id.
bool reinsertHeld(const ObjectRef& target, std::unique_ptr<MeshObject>& held, size_t index,
                  const std::string& description) {
    Scene* scene = target.scene();
    if (!scene || !held || scene->getObject(held->getId())) {
        LOG_WARNING("'{}': scene no longer exists or object {} is already there; skipped",
                    description, target.objectId.toString());
        return false;
    }
    return scene->insertObject(std::move(held), index) != nullptr;
}

// Take the target out of its scene into `held`; false when it is gone.
bool takeIntoHeld(const ObjectRef& target, std::unique_ptr<MeshObject>& held, size_t& index,
                  const std::string& description) {
    Scene* scene = target.scene();
    std::unique_ptr<MeshObject> object = scene ? scene->takeObject(target.objectId, &index) : nullptr;
    if (!object) {
        LOG_WARNING("'{}': object {} no longer exists; skipped", description,
                    target.objectId.toString());
        return false;
    }
    held = std::move(object);
    return true;
}

}  // namespace

AddObjectCommand::AddObjectCommand(QuadResolver& resolver, const UUID& sceneId,
                                   std::unique_ptr<MeshObject> object, size_t index)
    : target_{&resolver, sceneId, object ? object->getId() : UUID()}
    , held_(std::move(object))
    , index_(index)
{
}

bool AddObjectCommand::execute() {
    return reinsertHeld(target_, held_, index_, getDescription());
}

bool AddObjectCommand::undo() {
    return takeIntoHeld(target_, held_, index_, getDescription());
}

std::string AddObjectCommand::getDescription() const {
    return "Import 3D object";
}

void AddObjectCommand::collectMediaIds(std::unordered_set<UUID>& out) const {
    collectHeldMedia(held_, out);
}

void AddObjectCommand::remapMediaId(const UUID& from, const UUID& to) {
    remapHeldMedia(held_, from, to);
}

DeleteObjectCommand::DeleteObjectCommand(const ObjectRef& target)
    : target_(target)
{
}

bool DeleteObjectCommand::execute() {
    return takeIntoHeld(target_, held_, index_, getDescription());
}

bool DeleteObjectCommand::undo() {
    return reinsertHeld(target_, held_, index_, getDescription());
}

std::string DeleteObjectCommand::getDescription() const {
    return "Delete 3D object";
}

void DeleteObjectCommand::collectMediaIds(std::unordered_set<UUID>& out) const {
    collectHeldMedia(held_, out);
}

void DeleteObjectCommand::remapMediaId(const UUID& from, const UUID& to) {
    remapHeldMedia(held_, from, to);
}

namespace {

template <typename T>
std::unique_ptr<Command> makeObjectCommand(const char* description, const ObjectRef& target,
                                           T (*read)(const MeshObject&), const T& value,
                                           typename ObjectPropertyCommand<T>::Apply apply,
                                           bool mergeable) {
    const MeshObject* object = target.object();
    if (!object) return nullptr;
    return std::make_unique<ObjectPropertyCommand<T>>(description, target, read(*object), value,
                                                      apply, mergeable);
}

}  // namespace

std::unique_ptr<Command> makeSetObjectTransformCommand(const ObjectRef& target,
                                                       const glm::mat4& transform, bool mergeable) {
    return makeObjectCommand<glm::mat4>(
        "Transform 3D object", target, [](const MeshObject& o) { return o.getTransform(); },
        transform, [](MeshObject& o, const glm::mat4& v) { o.setTransform(v); }, mergeable);
}

std::unique_ptr<Command> makeSetObjectTextureCommand(const ObjectRef& target,
                                                     const TextureBinding& texture) {
    return makeObjectCommand<TextureBinding>(
        "Change 3D object media", target, [](const MeshObject& o) { return o.getTexture(); },
        texture, [](MeshObject& o, const TextureBinding& v) { o.getTexture() = v; }, false);
}

std::unique_ptr<Command> makeSetObjectVisibilityCommand(const ObjectRef& target, bool visible) {
    return makeObjectCommand<bool>(
        visible ? "Show 3D object" : "Hide 3D object", target,
        [](const MeshObject& o) { return o.isVisible(); }, visible,
        [](MeshObject& o, const bool& v) { o.setVisible(v); }, false);
}

std::unique_ptr<Command> makeSetObjectOpacityCommand(const ObjectRef& target, float opacity) {
    return makeObjectCommand<float>(
        "Change 3D object opacity", target, [](const MeshObject& o) { return o.getOpacity(); },
        opacity, [](MeshObject& o, const float& v) { o.setOpacity(v); }, true);
}

std::unique_ptr<Command> makeRenameObjectCommand(const ObjectRef& target, const std::string& name) {
    return makeObjectCommand<std::string>(
        "Rename 3D object", target, [](const MeshObject& o) { return o.getName(); }, name,
        [](MeshObject& o, const std::string& v) { o.setName(v); }, false);
}

// ---- Projector calibration -------------------------------------------------------

CalibrationEditCommand::CalibrationEditCommand(const OutputRef& target, CalibrationEdit edit)
    : target_(target)
    , edit_(std::move(edit))
{
}

bool CalibrationEditCommand::apply(const CalibrationSnapshot& snapshot) {
    const CalibrationTarget target = target_.target();
    if (!target) {
        LOG_WARNING("'{}': output {} no longer exists; skipped", edit_.description,
                    target_.outputId.toString());
        return false;
    }
    if (target.session) {
        target.session->restore(snapshot, *target.points, *target.projector);
    } else {
        *target.points = snapshot.points;
        *target.projector = snapshot.projector;
    }
    return true;
}

bool CalibrationEditCommand::execute() {
    if (firstExecute_) {
        firstExecute_ = false;
        if (target_.target()) return true;
        LOG_WARNING("'{}': output {} no longer exists; not recorded", edit_.description,
                    target_.outputId.toString());
        return false;
    }
    return apply(edit_.after);
}

bool CalibrationEditCommand::undo() {
    return apply(edit_.before);
}

bool CalibrationEditCommand::canMergeWith(const Command& other) const {
    auto* next = dynamic_cast<const CalibrationEditCommand*>(&other);
    return next && edit_.mergeKey != 0 && next->edit_.mergeKey == edit_.mergeKey &&
           target_.sameTarget(next->target_);
}

void CalibrationEditCommand::mergeWith(const Command& other) {
    if (auto* next = dynamic_cast<const CalibrationEditCommand*>(&other)) {
        edit_.after = next->edit_.after;
    }
}

} // namespace quadmapper
