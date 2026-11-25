#pragma once

#include "core/CommandHistory.hpp"
#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "Quad.hpp"
#include "MeshObject.hpp"
#include "CalibrationSession.hpp"
#include "Scene.hpp"
#include "Slice.hpp"
#include <functional>
#include <type_traits>
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

// ---- 3D objects (ADR-052, TD-16) ----------------------------------------------
// Same rule as quads: a command holds (scene id, object id) and resolves them through
// the QuadResolver on every execute/undo; a missing target is a logged no-op.

// Stable handle to one 3D object in one scene.
struct ObjectRef {
    QuadResolver* resolver = nullptr;
    UUID sceneId;
    UUID objectId;

    Scene* scene() const;
    MeshObject* object() const;  // nullptr when the scene or the object is gone

    bool sameTarget(const ObjectRef& other) const {
        return sceneId == other.sceneId && objectId == other.objectId;
    }
};

// Resolve ref for a command; logs and returns nullptr when the target is gone.
MeshObject* resolveObjectForCommand(const ObjectRef& ref, const std::string& commandDescription);

// Put a finished object (an import whose mesh has loaded) into a scene at `index`.
// Undo takes the object back out and keeps it whole (mesh included, no copy); redo
// puts the same object, same id, back where it was.
class AddObjectCommand : public Command {
public:
    AddObjectCommand(QuadResolver& resolver, const UUID& sceneId,
                     std::unique_ptr<MeshObject> object, size_t index);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;
    void collectMediaIds(std::unordered_set<UUID>& out) const override;
    void remapMediaId(const UUID& from, const UUID& to) override;

private:
    ObjectRef target_;
    std::unique_ptr<MeshObject> held_;  // the object while it is out of the scene
    size_t index_;
};

// Delete an object. The command keeps it whole while deleted; undo puts it back at
// its list position with the same id.
class DeleteObjectCommand : public Command {
public:
    explicit DeleteObjectCommand(const ObjectRef& target);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override;
    void collectMediaIds(std::unordered_set<UUID>& out) const override;
    void remapMediaId(const UUID& from, const UUID& to) override;

private:
    ObjectRef target_;
    std::unique_ptr<MeshObject> held_;
    size_t index_ = 0;
};

// Before/after value of one object property. Built by the make* functions below,
// which read the old value from the live object. A mergeable command folds into the
// previous one on the same object and property (a slider or drag is one step, as
// for corner moves).
template <typename T>
class ObjectPropertyCommand : public Command {
public:
    using Apply = void (*)(MeshObject&, const T&);

    ObjectPropertyCommand(std::string description, const ObjectRef& target, T oldValue,
                          T newValue, Apply apply, bool mergeable)
        : description_(std::move(description))
        , target_(target)
        , oldValue_(std::move(oldValue))
        , newValue_(std::move(newValue))
        , apply_(apply)
        , mergeable_(mergeable)
    {
    }

    bool execute() override { return applyValue(newValue_); }
    bool undo() override { return applyValue(oldValue_); }
    std::string getDescription() const override { return description_; }

    bool canMergeWith(const Command& other) const override {
        auto* next = dynamic_cast<const ObjectPropertyCommand<T>*>(&other);
        return mergeable_ && next && next->mergeable_ && next->apply_ == apply_ &&
               next->description_ == description_ && target_.sameTarget(next->target_);
    }
    void mergeWith(const Command& other) override {
        if (auto* next = dynamic_cast<const ObjectPropertyCommand<T>*>(&other)) {
            newValue_ = next->newValue_;
        }
    }

    // A texture binding step can put either of its sources back on the object.
    void collectMediaIds(std::unordered_set<UUID>& out) const override {
        if constexpr (std::is_same_v<T, TextureBinding>) {
            if (!oldValue_.mediaId.isNull()) out.insert(oldValue_.mediaId);
            if (!newValue_.mediaId.isNull()) out.insert(newValue_.mediaId);
        } else {
            (void)out;
        }
    }
    void remapMediaId(const UUID& from, const UUID& to) override {
        if constexpr (std::is_same_v<T, TextureBinding>) {
            if (oldValue_.mediaId == from) oldValue_.mediaId = to;
            if (newValue_.mediaId == from) newValue_.mediaId = to;
        } else {
            (void)from;
            (void)to;
        }
    }

private:
    bool applyValue(const T& value) {
        MeshObject* object = resolveObjectForCommand(target_, description_);
        if (!object) return false;
        apply_(*object, value);
        return true;
    }

    std::string description_;
    ObjectRef target_;
    T oldValue_;
    T newValue_;
    Apply apply_;
    bool mergeable_;
};

// Object property commands. Each returns nullptr when the object does not exist
// (CommandHistory::execute ignores a null command).
std::unique_ptr<Command> makeSetObjectTransformCommand(const ObjectRef& target,
                                                       const glm::mat4& transform, bool mergeable);
std::unique_ptr<Command> makeSetObjectTextureCommand(const ObjectRef& target,
                                                     const TextureBinding& texture);
std::unique_ptr<Command> makeSetObjectVisibilityCommand(const ObjectRef& target, bool visible);
std::unique_ptr<Command> makeSetObjectOpacityCommand(const ObjectRef& target, float opacity);
std::unique_ptr<Command> makeRenameObjectCommand(const ObjectRef& target, const std::string& name);

// ---- Projector calibration (ADR-052 §6, TD-17) ---------------------------------------
// Calibration lives on an output (OutputWindow), which this module cannot see, so a
// command reaches it by output id through an OutputResolver: the output's point list
// and projector, plus the calibration session while that output is being calibrated.
// An output that was removed (or re-created, which gives it a new id) resolves to
// nothing and the step is a logged no-op.

struct CalibrationTarget {
    std::vector<CalibrationPoint>* points = nullptr;
    ProjectorCamera* projector = nullptr;
    CalibrationSession* session = nullptr;  // only while this output is being calibrated

    explicit operator bool() const { return points != nullptr && projector != nullptr; }
};

// An output's slices and the canvas size they were made for (ADR-047).
struct OutputSlices {
    std::vector<Slice> slices;
    IVec2 canvas = IVec2(0, 0);
};

class OutputResolver {
public:
    virtual ~OutputResolver() = default;
    virtual CalibrationTarget findCalibration(const UUID& outputId) = 0;
    // The output's slices; false when there is no such output (or the resolver knows
    // no slices, as a calibration-only resolver does).
    virtual bool readSlices(const UUID& outputId, OutputSlices& out) {
        (void)outputId;
        (void)out;
        return false;
    }
    virtual bool writeSlices(const UUID& outputId, const OutputSlices& slices) {
        (void)outputId;
        (void)slices;
        return false;
    }
    // A scene's canvas changed from `from` to `to`: rescale the slices of every output
    // showing the scene now (rescaleSlices) from the canvas each was made for (`from`
    // when an output has none recorded).
    virtual void rescaleSceneSlices(const UUID& sceneId, IVec2 from, IVec2 to) {
        (void)sceneId;
        (void)from;
        (void)to;
    }
};

// Stable handle to one output's calibration.
struct OutputRef {
    OutputResolver* resolver = nullptr;
    UUID outputId;

    CalibrationTarget target() const {
        return resolver ? resolver->findCalibration(outputId) : CalibrationTarget{};
    }
    bool sameTarget(const OutputRef& other) const { return outputId == other.outputId; }
};

// One calibration edit (add, move, nudge, enable, delete, clear points; lens and mode
// changes; Reset view) as the whole before/after state, the solved projector included
// (see CalibrationSession::finishFrame). Moves of one point merge like corner moves.
class CalibrationEditCommand : public Command {
public:
    CalibrationEditCommand(const OutputRef& target, CalibrationEdit edit);

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override { return edit_.description; }

    bool canMergeWith(const Command& other) const override;
    void mergeWith(const Command& other) override;

    const UUID& getOutputId() const { return target_.outputId; }

private:
    bool apply(const CalibrationSnapshot& snapshot);

    OutputRef target_;
    CalibrationEdit edit_;
    // The edit is already on the output when the command is recorded (or merged): the
    // first execute only checks the output exists, so a drag in progress is not reset.
    bool firstExecute_ = true;
};

// ---- Output slices and scene canvases (ADR-047 part B) ----------------------------

// One slice edit on one output (add, remove, input rectangle, warp corner, blend
// edge, black level, auto-blend) as the output's whole slice list before and after.
// Consecutive edits with the same non-empty mergeKey on the same output merge into one
// undo step (a drag, a run of arrow-key nudges on one warp corner), unless the output's
// slices were rescaled to another canvas in between. A removed or re-created output
// resolves to nothing and the step is a logged no-op.
//
// The snapshots are in the canvas pixels of their moment. Takes, routing and canvas
// adoption rescale an output's slices without an undo step, so a snapshot is rescaled
// from its canvas to the canvas the output's slices are made for now before it is
// applied: the projector keeps showing the part of the picture the step recorded.
class SliceEditCommand : public Command {
public:
    SliceEditCommand(const OutputRef& target, std::string description, std::string mergeKey,
                     OutputSlices before, OutputSlices after);

    bool execute() override { return apply(after_); }
    bool undo() override { return apply(before_); }
    std::string getDescription() const override { return description_; }

    bool canMergeWith(const Command& other) const override;
    void mergeWith(const Command& other) override;

    const UUID& getOutputId() const { return target_.outputId; }

private:
    bool apply(const OutputSlices& slices);

    OutputRef target_;
    std::string description_;
    std::string mergeKey_;
    OutputSlices before_;
    OutputSlices after_;
};

// A scene's canvas size and whether the operator fixed it (Scene::isCanvasLocked).
struct CanvasSetting {
    IVec2 size = IVec2(DEFAULT_CANVAS_WIDTH, DEFAULT_CANVAS_HEIGHT);
    bool locked = false;
};

// Set a scene's canvas setting. Quads are normalized, so they keep their place; the
// slices of every output showing the scene when the step is done, undone or redone are
// rescaled to the new size through `outputs` (OutputResolver::rescaleSceneSlices), so
// an output routed to the scene after the edit follows its undo too. `outputs` may be
// null (no outputs to follow).
class SetSceneCanvasCommand : public Command {
public:
    SetSceneCanvasCommand(QuadResolver& resolver, OutputResolver* outputs, const UUID& sceneId,
                          CanvasSetting before, CanvasSetting after);

    bool execute() override { return apply(after_); }
    bool undo() override { return apply(before_); }
    std::string getDescription() const override { return "Canvas size"; }

private:
    bool apply(const CanvasSetting& setting);

    QuadResolver& resolver_;
    OutputResolver* outputs_;
    UUID sceneId_;
    CanvasSetting before_;
    CanvasSetting after_;
};

} // namespace quadmapper
