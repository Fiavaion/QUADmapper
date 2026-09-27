#include "ControlRegistry.hpp"

#include "OscPattern.hpp"
#include "core/Logger.hpp"

#include <cctype>

namespace quadmapper::control {

namespace {

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> segments;
    if (path.empty() || path[0] != '/') return segments;
    size_t start = 1;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        segments.push_back(path.substr(start, end - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return segments;
}

std::string capitalized(const std::string& text) {
    std::string out = text;
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

}  // namespace

bool ControlRegistry::equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

void ControlRegistry::addGlobal(const std::string& path, ControlTarget target) {
    globals_.emplace_back(path, std::move(target));
}

void ControlRegistry::addEntityTarget(const std::string& collection, ControlTarget target) {
    Collection* c = findCollection(collection);
    if (!c) {
        collections_.push_back(Collection{collection, capitalized(collection), {}, {}});
        c = &collections_.back();
    }
    c->actions.push_back(std::move(target));
}

void ControlRegistry::setEntities(const std::string& collection, std::vector<ControlEntity> entities) {
    Collection* c = findCollection(collection);
    if (!c) {
        collections_.push_back(Collection{collection, capitalized(collection), {}, {}});
        c = &collections_.back();
    }
    c->entities = std::move(entities);
}

const std::vector<ControlEntity>& ControlRegistry::entities(const std::string& collection) const {
    static const std::vector<ControlEntity> kNone;
    const Collection* c = findCollection(collection);
    return c ? c->entities : kNone;
}

ControlRegistry::Collection* ControlRegistry::findCollection(const std::string& name) {
    for (auto& c : collections_) {
        if (equalsIgnoreCase(c.name, name)) return &c;
    }
    return nullptr;
}

const ControlRegistry::Collection* ControlRegistry::findCollection(const std::string& name) const {
    for (const auto& c : collections_) {
        if (equalsIgnoreCase(c.name, name)) return &c;
    }
    return nullptr;
}

const ControlEntity* ControlRegistry::findEntity(const Collection& collection, const std::string& key,
                                                 const std::string& address) {
    if (key.empty()) return nullptr;
    for (const auto& entity : collection.entities) {
        if (equalsIgnoreCase(entity.id, key)) return &entity;  // UUID first
    }
    const ControlEntity* first = nullptr;
    bool ambiguous = false;
    for (const auto& entity : collection.entities) {
        if (!equalsIgnoreCase(entity.name, key)) continue;
        if (first) {
            ambiguous = true;
            break;
        }
        first = &entity;
    }
    if (ambiguous && warnedAmbiguous_.insert(address).second) {
        Logger::warning("Control: {} names more than one {} called '{}'; using the first. "
                        "Address it by id to be exact.", address, collection.name, key);
    }
    return first;
}

std::optional<ControlRegistry::Resolved> ControlRegistry::resolve(const std::string& path) {
    for (const auto& [globalPath, target] : globals_) {
        if (equalsIgnoreCase(globalPath, path)) {
            return Resolved{&target, std::string(), globalPath};
        }
    }

    const std::vector<std::string> segments = splitPath(path);
    if (segments.size() != 4 || !equalsIgnoreCase(segments[0], "qm")) return std::nullopt;
    Collection* collection = findCollection(segments[1]);
    if (!collection) return std::nullopt;
    const ControlTarget* action = nullptr;
    for (const auto& a : collection->actions) {
        if (equalsIgnoreCase(a.action, segments[3])) {
            action = &a;
            break;
        }
    }
    if (!action) return std::nullopt;
    const ControlEntity* entity = findEntity(*collection, segments[2], path);
    if (!entity) return std::nullopt;
    return Resolved{action, entity->id,
                    "/qm/" + collection->name + "/" + entity->id + "/" + action->action};
}

std::vector<ControlRegistry::Resolved> ControlRegistry::resolveAll(const std::string& address) {
    std::vector<Resolved> found;
    if (auto literal = resolve(address)) {
        found.push_back(std::move(*literal));
        return found;
    }
    if (!OscPattern::hasWildcards(address)) return found;
    const std::optional<OscPattern> pattern = OscPattern::compile(address);
    if (!pattern) return found;

    for (const auto& [globalPath, target] : globals_) {
        if (pattern->matches(globalPath)) found.push_back(Resolved{&target, std::string(), globalPath});
    }
    if (pattern->partCount() != 4 || !pattern->matchPart(0, "qm")) return found;
    for (const auto& collection : collections_) {
        if (!pattern->matchPart(1, collection.name)) continue;
        std::vector<const ControlTarget*> actions;
        for (const auto& action : collection.actions) {
            if (pattern->matchPart(3, action.action)) actions.push_back(&action);
        }
        if (actions.empty()) continue;
        // Only a spelled-out entity part may name UUIDs: "F*" or "[a-f]*" would
        // otherwise pick entities whose random id happens to start with that letter.
        const bool idsToo = pattern->isExplicitPart(2);
        for (const auto& entity : collection.entities) {
            if (!pattern->matchPart(2, entity.name) && !(idsToo && pattern->matchPart(2, entity.id))) continue;
            for (const ControlTarget* action : actions) {
                found.push_back(Resolved{action, entity.id,
                                         "/qm/" + collection.name + "/" + entity.id + "/" + action->action});
            }
        }
    }
    return found;
}

bool ControlRegistry::isValidAddressPart(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F) return false;
        switch (c) {
            case '#': case '*': case ',': case '/': case '?':
            case '[': case ']': case '{': case '}':
                return false;
            default:
                break;
        }
    }
    return true;
}

std::vector<ControlRegistry::FeedbackTarget> ControlRegistry::feedbackTargets(bool preferNames) const {
    auto hasFeedback = [](const ControlTarget& t) { return t.read && t.kind != TargetKind::Trigger; };
    std::vector<FeedbackTarget> list;
    for (const auto& [path, target] : globals_) {
        if (hasFeedback(target)) list.push_back({path, path, &target, std::string()});
    }
    for (const auto& collection : collections_) {
        for (const auto& entity : collection.entities) {
            std::string key = entity.id;
            if (preferNames && isValidAddressPart(entity.name)) {
                size_t clashes = 0;
                for (const auto& other : collection.entities) {
                    if (equalsIgnoreCase(other.name, entity.name)) ++clashes;
                    if (equalsIgnoreCase(other.id, entity.name)) ++clashes;
                }
                if (clashes == 1) key = entity.name;  // only itself
            }
            for (const auto& action : collection.actions) {
                if (!hasFeedback(action)) continue;
                list.push_back({"/qm/" + collection.name + "/" + entity.id + "/" + action.action,
                                "/qm/" + collection.name + "/" + key + "/" + action.action, &action,
                                entity.id});
            }
        }
    }
    return list;
}

std::string ControlRegistry::labelFor(const Collection& collection, const ControlEntity& entity,
                                      const ControlTarget& target) {
    return collection.label + " \"" + entity.name + "\" " + target.action;
}

std::vector<ControlRegistry::ListedTarget> ControlRegistry::listTargets() const {
    std::vector<ListedTarget> list;
    for (const auto& [path, target] : globals_) {
        if (target.learnable) list.push_back({path, path, target.kind, target.min, target.max});
    }
    for (const auto& collection : collections_) {
        for (const auto& entity : collection.entities) {
            for (const auto& action : collection.actions) {
                if (!action.learnable) continue;
                list.push_back({"/qm/" + collection.name + "/" + entity.id + "/" + action.action,
                                labelFor(collection, entity, action), action.kind, action.min,
                                action.max});
            }
        }
    }
    return list;
}

std::optional<std::string> ControlRegistry::describe(const std::string& path) {
    for (const auto& [globalPath, target] : globals_) {
        if (equalsIgnoreCase(globalPath, path)) return globalPath;
    }
    const std::vector<std::string> segments = splitPath(path);
    if (segments.size() != 4 || !equalsIgnoreCase(segments[0], "qm")) return std::nullopt;
    const Collection* collection = findCollection(segments[1]);
    if (!collection) return std::nullopt;
    for (const auto& action : collection->actions) {
        if (!equalsIgnoreCase(action.action, segments[3])) continue;
        for (const auto& entity : collection->entities) {
            if (equalsIgnoreCase(entity.id, segments[2])) return labelFor(*collection, entity, action);
        }
        for (const auto& entity : collection->entities) {
            if (equalsIgnoreCase(entity.name, segments[2])) return labelFor(*collection, entity, action);
        }
    }
    return std::nullopt;
}

}  // namespace quadmapper::control
