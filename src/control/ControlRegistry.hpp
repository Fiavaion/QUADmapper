#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace quadmapper::control {

// Kind of a control target (ADR-048 §2).
//   Trigger - an action (play, take). Fires on a message with no argument or an
//             argument >= 0.5; ignores < 0.5 (a button's release).
//   Float   - a value clamped into [min, max] (opacity 0-1, speed 0.1-4).
//   Bool    - on when the argument is >= 0.5; a message with no argument toggles.
//   Index   - needs a numeric argument, rounded to an integer >= min (scene take n).
enum class TargetKind { Trigger, Float, Bool, Index };

// One addressable control. `apply` and `read` run on the main thread and receive
// the entity id (the UUID string of the quad/scene/source; empty for a global
// target). apply's value is already clamped/decoded for the kind: Trigger gets 1,
// Bool gets 0 or 1, Float the clamped value, Index the rounded integer.
struct ControlTarget {
    std::string action;              // entity targets: last path segment ("opacity")
    TargetKind kind = TargetKind::Trigger;
    float min = 0.0f;
    float max = 1.0f;
    bool learnable = true;           // listed in the Control panel for MIDI learn
    std::function<void(const std::string& entityId, float value)> apply;
    std::function<std::optional<float>(const std::string& entityId)> read;  // optional
};

// Something a path can name: a quad, scene or source.
struct ControlEntity {
    std::string id;    // UUID string
    std::string name;
};

// The control-target registry (ADR-048 §2). Targets are registered once; the
// entity lists (quads, scenes, sources of the open project) are refreshed by the
// owner whenever the project may have changed. Adding a target is one call:
//   registry.addGlobal("/qm/blackout", {...});
//   registry.addEntityTarget("source", {"volume", TargetKind::Float, 0, 1, ...});
// Paths:
//   global:  the registered path, e.g. /qm/blackout, /qm/scene/take
//   entity:  /qm/<collection>/<id or name>/<action>
// Fixed segments match case-insensitively. An entity segment is tried as a UUID
// first, then as a case-insensitive name; a name shared by several entities uses
// the first in list order and logs one warning per address.
class ControlRegistry {
public:
    struct Resolved {
        const ControlTarget* target = nullptr;
        std::string entityId;        // empty for a global target
        std::string canonicalPath;   // the path with the entity's UUID
    };

    struct ListedTarget {
        std::string path;            // canonical (UUID) path
        std::string label;           // human-readable, e.g. Quad "Left wall" opacity
        TargetKind kind = TargetKind::Trigger;
        float min = 0.0f;
        float max = 1.0f;
    };

    void addGlobal(const std::string& path, ControlTarget target);
    void addEntityTarget(const std::string& collection, ControlTarget target);

    // Replace a collection's entities (list order = name-resolution order).
    void setEntities(const std::string& collection, std::vector<ControlEntity> entities);
    const std::vector<ControlEntity>& entities(const std::string& collection) const;

    std::optional<Resolved> resolve(const std::string& path);

    // Every learnable target that exists now, by canonical path, in registration
    // order: globals first, then each collection's entities x actions.
    std::vector<ListedTarget> listTargets() const;

    // Label for a path, or nullopt when it does not resolve now (a deleted quad).
    std::optional<std::string> describe(const std::string& path);

    // Case-insensitive ASCII string equality.
    static bool equalsIgnoreCase(const std::string& a, const std::string& b);

private:
    struct Collection {
        std::string name;                        // "quad", "scene", "source"
        std::string label;                       // "Quad", ...
        std::vector<ControlTarget> actions;
        std::vector<ControlEntity> entities;
    };

    Collection* findCollection(const std::string& name);
    const Collection* findCollection(const std::string& name) const;
    const ControlEntity* findEntity(const Collection& collection, const std::string& key,
                                    const std::string& address);
    static std::string labelFor(const Collection& collection, const ControlEntity& entity,
                                const ControlTarget& target);

    std::vector<std::pair<std::string, ControlTarget>> globals_;
    std::vector<Collection> collections_;
    std::unordered_set<std::string> warnedAmbiguous_;  // addresses already warned about
};

}  // namespace quadmapper::control
