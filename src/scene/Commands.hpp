#pragma once

#include "core/CommandHistory.hpp"
#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "Quad.hpp"
#include <functional>
#include <string>
#include <memory>
#include <array>

namespace quadmapper {

// Forward declarations
class QuadManager;

// MoveCornerCommand - handles quad corner movement with merge support
class MoveCornerCommand : public Command {
public:
    MoveCornerCommand(Quad* quad, int cornerIndex, const Vec2& oldPosition, const Vec2& newPosition);

    void execute() override;
    void undo() override;
    std::string getDescription() const override;

    bool canMergeWith(const Command& other) const override;
    void mergeWith(const Command& other) override;

private:
    Quad* quad_;
    int cornerIndex_;
    Vec2 oldPosition_;
    Vec2 newPosition_;
};

// MoveMeshVertexCommand - handles mesh vertex movement
class MoveMeshVertexCommand : public Command {
public:
    MoveMeshVertexCommand(Quad* quad, int vertexIndex, const Vec2& oldOffset, const Vec2& newOffset);

    void execute() override;
    void undo() override;
    std::string getDescription() const override;

    bool canMergeWith(const Command& other) const override;
    void mergeWith(const Command& other) override;

private:
    Quad* quad_;
    int vertexIndex_;
    Vec2 oldOffset_;
    Vec2 newOffset_;
};

// SetPropertyCommand - generic property change command using callbacks
template<typename T>
class SetPropertyCommand : public Command {
public:
    using GetterFunc = std::function<T()>;
    using SetterFunc = std::function<void(const T&)>;

    SetPropertyCommand(const std::string& propertyName,
                       const T& oldValue,
                       const T& newValue,
                       SetterFunc setter)
        : propertyName_(propertyName)
        , oldValue_(oldValue)
        , newValue_(newValue)
        , setter_(std::move(setter))
    {
    }

    void execute() override {
        setter_(newValue_);
    }

    void undo() override {
        setter_(oldValue_);
    }

    std::string getDescription() const override {
        return "Change " + propertyName_;
    }

private:
    std::string propertyName_;
    T oldValue_;
    T newValue_;
    SetterFunc setter_;
};

// CreateQuadCommand - handles quad creation
class CreateQuadCommand : public Command {
public:
    explicit CreateQuadCommand(QuadManager* manager, const std::string& name = "");

    void execute() override;
    void undo() override;
    std::string getDescription() const override;

    // Get the created quad's ID (valid after execute)
    const UUID& getCreatedQuadId() const { return createdQuadId_; }

private:
    QuadManager* manager_;
    std::string name_;
    UUID createdQuadId_;

    // Stored data for undo/redo
    struct QuadData;
    std::unique_ptr<QuadData> storedData_;
};

// DeleteQuadCommand - handles quad deletion
class DeleteQuadCommand : public Command {
public:
    DeleteQuadCommand(QuadManager* manager, const UUID& quadId);

    void execute() override;
    void undo() override;
    std::string getDescription() const override;

private:
    QuadManager* manager_;
    UUID quadId_;

    // Stored data for recreation
    struct QuadData;
    std::unique_ptr<QuadData> storedData_;
};

// SetQuadVisibilityCommand
class SetQuadVisibilityCommand : public Command {
public:
    SetQuadVisibilityCommand(Quad* quad, bool newVisible);

    void execute() override;
    void undo() override;
    std::string getDescription() const override;

private:
    Quad* quad_;
    bool oldVisible_;
    bool newVisible_;
};

// SetQuadLockedCommand
class SetQuadLockedCommand : public Command {
public:
    SetQuadLockedCommand(Quad* quad, bool newLocked);

    void execute() override;
    void undo() override;
    std::string getDescription() const override;

private:
    Quad* quad_;
    bool oldLocked_;
    bool newLocked_;
};

// SetQuadOpacityCommand
class SetQuadOpacityCommand : public Command {
public:
    SetQuadOpacityCommand(Quad* quad, float newOpacity);

    void execute() override;
    void undo() override;
    std::string getDescription() const override;

    bool canMergeWith(const Command& other) const override;
    void mergeWith(const Command& other) override;

private:
    Quad* quad_;
    float oldOpacity_;
    float newOpacity_;
};

// RenameQuadCommand
class RenameQuadCommand : public Command {
public:
    RenameQuadCommand(Quad* quad, const std::string& newName);

    void execute() override;
    void undo() override;
    std::string getDescription() const override;

private:
    Quad* quad_;
    std::string oldName_;
    std::string newName_;
};

// SetMeshResolutionCommand
class SetMeshResolutionCommand : public Command {
public:
    SetMeshResolutionCommand(Quad* quad, int newResolution);

    void execute() override;
    void undo() override;
    std::string getDescription() const override;

private:
    Quad* quad_;
    int oldResolution_;
    int newResolution_;
    std::vector<Vec2> oldMeshVertices_;
};

// MoveQuadCommand - moves all corners together
class MoveQuadCommand : public Command {
public:
    MoveQuadCommand(Quad* quad, const Vec2& delta);

    void execute() override;
    void undo() override;
    std::string getDescription() const override;

    bool canMergeWith(const Command& other) const override;
    void mergeWith(const Command& other) override;

private:
    Quad* quad_;
    std::array<Vec2, 4> oldCorners_;
    Vec2 totalDelta_;
};

} // namespace quadmapper
