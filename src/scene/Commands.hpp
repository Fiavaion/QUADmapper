#pragma once

#include "core/CommandHistory.hpp"
#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "Quad.hpp"
#include <functional>
#include <string>
#include <memory>
#include <optional>
#include <array>
#include <vector>

namespace quadmapper {

class Scene;

// Undo commands never hold Quad* / QuadManager* / Scene*: a scene (and every quad in
// it) can be destroyed by delete, New or Load while its commands sit in the history.
// Commands store stable ids and resolve their target through a QuadResolver on every
// execute/undo; a missing target makes the command a logged no-op that returns false.
class QuadResolver {
public:
    virtual ~QuadResolver() = default;
    virtual Scene* findScene(const UUID& sceneId) = 0;
};

// Resolves against a live scene list (the editor's EditorState::scenes).
class SceneListResolver : public QuadResolver {
public:
    explicit SceneListResolver(std::vector<std::unique_ptr<Scene>>& scenes) : scenes_(scenes) {}
    Scene* findScene(const UUID& sceneId) override;

private:
    std::vector<std::unique_ptr<Scene>>& scenes_;
};

// Stable handle to one quad in one scene.
struct QuadRef {
    QuadResolver* resolver = nullptr;
    UUID sceneId;
    UUID quadId;

    Scene* scene() const;
    Quad* quad() const;  // nullptr when the scene or the quad is gone

    bool sameTarget(const QuadRef& other) const {
        return sceneId == other.sceneId && quadId == other.quadId;
    }
};

// Resolve ref for a command; logs and returns nullptr when the target is gone.
Quad* resolveForCommand(const QuadRef& ref, const std::string& commandDescription);

// MoveCornerCommand - handles quad corner movement with merge support
class MoveCornerCommand : public Command {
public:
    MoveCornerCommand(const QuadRef& target, int cornerIndex, const Vec2& oldPosition, const Vec2& newPosition);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;

    bool canMergeWith(const Command& other) const override;
    void mergeWith(const Command& other) override;

private:
    QuadRef target_;
    int cornerIndex_;
    Vec2 oldPosition_;
    Vec2 newPosition_;
};

// MoveMeshVertexCommand - handles mesh vertex movement
class MoveMeshVertexCommand : public Command {
public:
    MoveMeshVertexCommand(const QuadRef& target, int vertexIndex, const Vec2& oldOffset, const Vec2& newOffset);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;

    bool canMergeWith(const Command& other) const override;
    void mergeWith(const Command& other) override;

private:
    QuadRef target_;
    int vertexIndex_;
    Vec2 oldOffset_;
    Vec2 newOffset_;
};

// MoveGridHandleCommand - one control vertex's tangent handles (ADR-049 §3) before
// and after moving `handle`. Consecutive moves of the same handle merge.
class MoveGridHandleCommand : public Command {
public:
    MoveGridHandleCommand(const QuadRef& target, int vertexIndex, int handle,
                          const VertexTangents& oldTangents, const VertexTangents& newTangents);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;

    bool canMergeWith(const Command& other) const override;
    void mergeWith(const Command& other) override;

private:
    QuadRef target_;
    int vertexIndex_;
    int handle_;
    VertexTangents oldTangents_;
    VertexTangents newTangents_;
};

// QuadPropertyCommand - generic before/after value of one quad, applied by callback.
// The callback receives the resolved quad, so it must not capture a Quad*.
template<typename T>
class QuadPropertyCommand : public Command {
public:
    using ApplyFunc = std::function<void(Quad&, const T&)>;

    QuadPropertyCommand(const std::string& propertyName,
                        const QuadRef& target,
                        const T& oldValue,
                        const T& newValue,
                        ApplyFunc apply)
        : propertyName_(propertyName)
        , target_(target)
        , oldValue_(oldValue)
        , newValue_(newValue)
        , apply_(std::move(apply))
    {
    }

    bool execute() override { return applyValue(newValue_); }
    bool undo() override { return applyValue(oldValue_); }

    std::string getDescription() const override {
        return "Change " + propertyName_;
    }

private:
    bool applyValue(const T& value) {
        Quad* quad = resolveForCommand(target_, getDescription());
        if (!quad) return false;
        apply_(*quad, value);
        return true;
    }

    std::string propertyName_;
    QuadRef target_;
    T oldValue_;
    T newValue_;
    ApplyFunc apply_;
};

// CreateQuadCommand - handles quad creation. Redo restores the same id and state.
class CreateQuadCommand : public Command {
public:
    CreateQuadCommand(QuadResolver& resolver, const UUID& sceneId, const std::string& name = "");

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;
    void collectMediaIds(std::unordered_set<UUID>& out) const override;

    // Get the created quad's ID (valid after execute)
    const UUID& getCreatedQuadId() const { return created_.quadId; }

private:
    QuadRef created_;
    std::string name_;
    std::optional<Quad> snapshot_;  // full state captured on undo, restored on redo
};

// DeleteQuadCommand - deletes through Scene::deleteQuad (group cleanup). Undo
// re-inserts the full-state snapshot with the same id, z-order and group membership.
class DeleteQuadCommand : public Command {
public:
    explicit DeleteQuadCommand(const QuadRef& target);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;
    void collectMediaIds(std::unordered_set<UUID>& out) const override;

private:
    QuadRef target_;
    std::optional<Quad> snapshot_;
    std::vector<UUID> groupIds_;
};

// DuplicateQuadCommand - full-state duplicate of a quad (QuadManager::duplicateQuad).
// Redo restores the duplicate with the same id, so later commands on it stay valid.
class DuplicateQuadCommand : public Command {
public:
    explicit DuplicateQuadCommand(const QuadRef& source);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;
    void collectMediaIds(std::unordered_set<UUID>& out) const override;

    // The duplicate's ID, fixed at construction (the quad exists once execute succeeds)
    const UUID& getDuplicateId() const { return duplicate_.quadId; }

private:
    QuadRef source_;
    QuadRef duplicate_;
    std::optional<Quad> snapshot_;
};

// SetQuadVisibilityCommand
class SetQuadVisibilityCommand : public Command {
public:
    SetQuadVisibilityCommand(const QuadRef& target, bool newVisible);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;

private:
    QuadRef target_;
    bool oldVisible_;
    bool newVisible_;
};

// SetQuadLockedCommand
class SetQuadLockedCommand : public Command {
public:
    SetQuadLockedCommand(const QuadRef& target, bool newLocked);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;

private:
    QuadRef target_;
    bool oldLocked_;
    bool newLocked_;
};

// SetQuadOpacityCommand
class SetQuadOpacityCommand : public Command {
public:
    SetQuadOpacityCommand(const QuadRef& target, float newOpacity);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;

    bool canMergeWith(const Command& other) const override;
    void mergeWith(const Command& other) override;

private:
    QuadRef target_;
    float oldOpacity_;
    float newOpacity_;
};

// RenameQuadCommand
class RenameQuadCommand : public Command {
public:
    RenameQuadCommand(const QuadRef& target, const std::string& newName);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;

private:
    QuadRef target_;
    std::string oldName_;
    std::string newName_;
};

// SetMeshResolutionCommand
class SetMeshResolutionCommand : public Command {
public:
    SetMeshResolutionCommand(const QuadRef& target, int newResolution);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;

private:
    QuadRef target_;
    int oldResolution_;
    int newResolution_;
    std::vector<Vec2> oldMeshVertices_;
};

// MoveQuadCommand - moves all corners together
class MoveQuadCommand : public Command {
public:
    MoveQuadCommand(const QuadRef& target, const Vec2& delta);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;

    bool canMergeWith(const Command& other) const override;
    void mergeWith(const Command& other) override;

private:
    QuadRef target_;
    std::array<Vec2, 4> oldCorners_;
    Vec2 totalDelta_;
};

} // namespace quadmapper
