#include "Commands.hpp"
#include "Quad.hpp"
#include "QuadManager.hpp"
#include "core/Logger.hpp"

namespace quadmapper {

// QuadData for storing full quad state
struct CreateQuadCommand::QuadData {
    std::string name;
    int zOrder = 0;
    bool visible = true;
    bool locked = false;
    float opacity = 1.0f;
    std::array<Vec2, 4> corners;
    int meshResolution = 16;
    MeshMode meshMode = MeshMode::CORNERS_ONLY;
    std::vector<Vec2> meshVertices;
    TextureBinding texture;
};

struct DeleteQuadCommand::QuadData {
    UUID id;
    std::string name;
    int zOrder = 0;
    bool visible = true;
    bool locked = false;
    float opacity = 1.0f;
    std::array<Vec2, 4> corners;
    int meshResolution = 16;
    MeshMode meshMode = MeshMode::CORNERS_ONLY;
    std::vector<Vec2> meshVertices;
    TextureBinding texture;
};

// MoveCornerCommand implementation
MoveCornerCommand::MoveCornerCommand(Quad* quad, int cornerIndex, const Vec2& oldPosition, const Vec2& newPosition)
    : quad_(quad)
    , cornerIndex_(cornerIndex)
    , oldPosition_(oldPosition)
    , newPosition_(newPosition)
{
}

void MoveCornerCommand::execute() {
    if (quad_) {
        quad_->getGeometry().setCorner(cornerIndex_, newPosition_);
    }
}

void MoveCornerCommand::undo() {
    if (quad_) {
        quad_->getGeometry().setCorner(cornerIndex_, oldPosition_);
    }
}

std::string MoveCornerCommand::getDescription() const {
    static const char* cornerNames[] = {"top-left", "top-right", "bottom-right", "bottom-left"};
    return std::string("Move ") + cornerNames[cornerIndex_] + " corner";
}

bool MoveCornerCommand::canMergeWith(const Command& other) const {
    auto* otherMove = dynamic_cast<const MoveCornerCommand*>(&other);
    if (!otherMove) return false;
    return quad_ == otherMove->quad_ && cornerIndex_ == otherMove->cornerIndex_;
}

void MoveCornerCommand::mergeWith(const Command& other) {
    auto* otherMove = dynamic_cast<const MoveCornerCommand*>(&other);
    if (otherMove) {
        newPosition_ = otherMove->newPosition_;
    }
}

// MoveMeshVertexCommand implementation
MoveMeshVertexCommand::MoveMeshVertexCommand(Quad* quad, int vertexIndex, const Vec2& oldOffset, const Vec2& newOffset)
    : quad_(quad)
    , vertexIndex_(vertexIndex)
    , oldOffset_(oldOffset)
    , newOffset_(newOffset)
{
}

void MoveMeshVertexCommand::execute() {
    if (quad_) {
        quad_->getGeometry().setMeshVertexByIndex(vertexIndex_, newOffset_);
    }
}

void MoveMeshVertexCommand::undo() {
    if (quad_) {
        quad_->getGeometry().setMeshVertexByIndex(vertexIndex_, oldOffset_);
    }
}

std::string MoveMeshVertexCommand::getDescription() const {
    return "Move mesh vertex";
}

bool MoveMeshVertexCommand::canMergeWith(const Command& other) const {
    auto* otherMove = dynamic_cast<const MoveMeshVertexCommand*>(&other);
    if (!otherMove) return false;
    return quad_ == otherMove->quad_ && vertexIndex_ == otherMove->vertexIndex_;
}

void MoveMeshVertexCommand::mergeWith(const Command& other) {
    auto* otherMove = dynamic_cast<const MoveMeshVertexCommand*>(&other);
    if (otherMove) {
        newOffset_ = otherMove->newOffset_;
    }
}

// CreateQuadCommand implementation
CreateQuadCommand::CreateQuadCommand(QuadManager* manager, const std::string& name)
    : manager_(manager)
    , name_(name)
    , storedData_(std::make_unique<QuadData>())
{
}

void CreateQuadCommand::execute() {
    if (!manager_) return;

    Quad* quad;
    if (createdQuadId_.isNull()) {
        // First execution - create new quad
        if (name_.empty()) {
            quad = manager_->createQuad();
        } else {
            quad = manager_->createQuad(name_);
        }
        if (quad) {
            createdQuadId_ = quad->getId();
        }
    } else {
        // Re-execution (redo) - restore the quad
        if (name_.empty()) {
            quad = manager_->createQuad();
        } else {
            quad = manager_->createQuad(storedData_->name);
        }
        if (quad) {
            // Restore stored state
            quad->setZOrder(storedData_->zOrder);
            quad->setVisible(storedData_->visible);
            quad->setLocked(storedData_->locked);
            quad->setOpacity(storedData_->opacity);
            quad->getGeometry().setCorners(storedData_->corners);
            quad->getGeometry().setMeshResolution(storedData_->meshResolution);
            quad->getGeometry().setMeshMode(storedData_->meshMode);
            quad->getGeometry().setMeshVertices(storedData_->meshVertices);
            quad->getTexture() = storedData_->texture;
            // Note: We can't restore the original ID, but that's acceptable
        }
    }
}

void CreateQuadCommand::undo() {
    if (!manager_) return;

    // Store the quad's current state before deleting
    Quad* quad = manager_->getQuad(createdQuadId_);
    if (quad) {
        storedData_->name = quad->getName();
        storedData_->zOrder = quad->getZOrder();
        storedData_->visible = quad->isVisible();
        storedData_->locked = quad->isLocked();
        storedData_->opacity = quad->getOpacity();
        storedData_->corners = quad->getGeometry().getCorners();
        storedData_->meshResolution = quad->getGeometry().getMeshResolution();
        storedData_->meshMode = quad->getGeometry().getMeshMode();
        storedData_->meshVertices = quad->getGeometry().getMeshVertices();
        storedData_->texture = quad->getTexture();
    }

    manager_->deleteQuad(createdQuadId_);
}

std::string CreateQuadCommand::getDescription() const {
    return "Create quad";
}

// DeleteQuadCommand implementation
DeleteQuadCommand::DeleteQuadCommand(QuadManager* manager, const UUID& quadId)
    : manager_(manager)
    , quadId_(quadId)
    , storedData_(std::make_unique<QuadData>())
{
    // Store the quad's state immediately
    if (manager_) {
        const Quad* quad = manager_->getQuad(quadId_);
        if (quad) {
            storedData_->id = quad->getId();
            storedData_->name = quad->getName();
            storedData_->zOrder = quad->getZOrder();
            storedData_->visible = quad->isVisible();
            storedData_->locked = quad->isLocked();
            storedData_->opacity = quad->getOpacity();
            storedData_->corners = quad->getGeometry().getCorners();
            storedData_->meshResolution = quad->getGeometry().getMeshResolution();
            storedData_->meshMode = quad->getGeometry().getMeshMode();
            storedData_->meshVertices = quad->getGeometry().getMeshVertices();
            storedData_->texture = quad->getTexture();
        }
    }
}

void DeleteQuadCommand::execute() {
    if (!manager_) return;

    // Store current state in case we need to restore
    const Quad* quad = manager_->getQuad(quadId_);
    if (quad) {
        storedData_->id = quad->getId();
        storedData_->name = quad->getName();
        storedData_->zOrder = quad->getZOrder();
        storedData_->visible = quad->isVisible();
        storedData_->locked = quad->isLocked();
        storedData_->opacity = quad->getOpacity();
        storedData_->corners = quad->getGeometry().getCorners();
        storedData_->meshResolution = quad->getGeometry().getMeshResolution();
        storedData_->meshMode = quad->getGeometry().getMeshMode();
        storedData_->meshVertices = quad->getGeometry().getMeshVertices();
        storedData_->texture = quad->getTexture();
    }

    manager_->deleteQuad(quadId_);
}

void DeleteQuadCommand::undo() {
    if (!manager_) return;

    // Recreate the quad
    Quad* quad = manager_->createQuad(storedData_->name);
    if (quad) {
        quad->setZOrder(storedData_->zOrder);
        quad->setVisible(storedData_->visible);
        quad->setLocked(storedData_->locked);
        quad->setOpacity(storedData_->opacity);
        quad->getGeometry().setCorners(storedData_->corners);
        quad->getGeometry().setMeshResolution(storedData_->meshResolution);
        quad->getGeometry().setMeshMode(storedData_->meshMode);
        quad->getGeometry().setMeshVertices(storedData_->meshVertices);
        quad->getTexture() = storedData_->texture;

        // Update the stored ID to match the new quad
        quadId_ = quad->getId();
    }
}

std::string DeleteQuadCommand::getDescription() const {
    return "Delete quad";
}

// SetQuadVisibilityCommand implementation
SetQuadVisibilityCommand::SetQuadVisibilityCommand(Quad* quad, bool newVisible)
    : quad_(quad)
    , oldVisible_(quad ? quad->isVisible() : true)
    , newVisible_(newVisible)
{
}

void SetQuadVisibilityCommand::execute() {
    if (quad_) {
        quad_->setVisible(newVisible_);
    }
}

void SetQuadVisibilityCommand::undo() {
    if (quad_) {
        quad_->setVisible(oldVisible_);
    }
}

std::string SetQuadVisibilityCommand::getDescription() const {
    return newVisible_ ? "Show quad" : "Hide quad";
}

// SetQuadLockedCommand implementation
SetQuadLockedCommand::SetQuadLockedCommand(Quad* quad, bool newLocked)
    : quad_(quad)
    , oldLocked_(quad ? quad->isLocked() : false)
    , newLocked_(newLocked)
{
}

void SetQuadLockedCommand::execute() {
    if (quad_) {
        quad_->setLocked(newLocked_);
    }
}

void SetQuadLockedCommand::undo() {
    if (quad_) {
        quad_->setLocked(oldLocked_);
    }
}

std::string SetQuadLockedCommand::getDescription() const {
    return newLocked_ ? "Lock quad" : "Unlock quad";
}

// SetQuadOpacityCommand implementation
SetQuadOpacityCommand::SetQuadOpacityCommand(Quad* quad, float newOpacity)
    : quad_(quad)
    , oldOpacity_(quad ? quad->getOpacity() : 1.0f)
    , newOpacity_(newOpacity)
{
}

void SetQuadOpacityCommand::execute() {
    if (quad_) {
        quad_->setOpacity(newOpacity_);
    }
}

void SetQuadOpacityCommand::undo() {
    if (quad_) {
        quad_->setOpacity(oldOpacity_);
    }
}

std::string SetQuadOpacityCommand::getDescription() const {
    return "Change opacity";
}

bool SetQuadOpacityCommand::canMergeWith(const Command& other) const {
    auto* otherCmd = dynamic_cast<const SetQuadOpacityCommand*>(&other);
    return otherCmd && quad_ == otherCmd->quad_;
}

void SetQuadOpacityCommand::mergeWith(const Command& other) {
    auto* otherCmd = dynamic_cast<const SetQuadOpacityCommand*>(&other);
    if (otherCmd) {
        newOpacity_ = otherCmd->newOpacity_;
    }
}

// RenameQuadCommand implementation
RenameQuadCommand::RenameQuadCommand(Quad* quad, const std::string& newName)
    : quad_(quad)
    , oldName_(quad ? quad->getName() : "")
    , newName_(newName)
{
}

void RenameQuadCommand::execute() {
    if (quad_) {
        quad_->setName(newName_);
    }
}

void RenameQuadCommand::undo() {
    if (quad_) {
        quad_->setName(oldName_);
    }
}

std::string RenameQuadCommand::getDescription() const {
    return "Rename quad";
}

// SetMeshResolutionCommand implementation
SetMeshResolutionCommand::SetMeshResolutionCommand(Quad* quad, int newResolution)
    : quad_(quad)
    , oldResolution_(quad ? quad->getGeometry().getMeshResolution() : 16)
    , newResolution_(newResolution)
{
    if (quad_) {
        oldMeshVertices_ = quad_->getGeometry().getMeshVertices();
    }
}

void SetMeshResolutionCommand::execute() {
    if (quad_) {
        quad_->getGeometry().setMeshResolution(newResolution_);
    }
}

void SetMeshResolutionCommand::undo() {
    if (quad_) {
        quad_->getGeometry().setMeshResolution(oldResolution_);
        quad_->getGeometry().setMeshVertices(oldMeshVertices_);
    }
}

std::string SetMeshResolutionCommand::getDescription() const {
    return "Change mesh resolution";
}

// MoveQuadCommand implementation
MoveQuadCommand::MoveQuadCommand(Quad* quad, const Vec2& delta)
    : quad_(quad)
    , totalDelta_(delta)
{
    if (quad_) {
        oldCorners_ = quad_->getGeometry().getCorners();
    }
}

void MoveQuadCommand::execute() {
    if (quad_) {
        auto& geometry = quad_->getGeometry();
        auto corners = oldCorners_;
        for (auto& corner : corners) {
            corner += totalDelta_;
        }
        geometry.setCorners(corners);
    }
}

void MoveQuadCommand::undo() {
    if (quad_) {
        quad_->getGeometry().setCorners(oldCorners_);
    }
}

std::string MoveQuadCommand::getDescription() const {
    return "Move quad";
}

bool MoveQuadCommand::canMergeWith(const Command& other) const {
    auto* otherMove = dynamic_cast<const MoveQuadCommand*>(&other);
    return otherMove && quad_ == otherMove->quad_;
}

void MoveQuadCommand::mergeWith(const Command& other) {
    auto* otherMove = dynamic_cast<const MoveQuadCommand*>(&other);
    if (otherMove) {
        totalDelta_ += otherMove->totalDelta_;
    }
}

} // namespace quadmapper
