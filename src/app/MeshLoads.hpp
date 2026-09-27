#pragma once

// 3D object meshes are parsed off the render thread (TD-16a). Importing an OBJ or
// opening a show puts the object in its scene at once, with an empty mesh, and queues
// the file on EditorState::objLoads; the object shows as "loading..." until the frame
// boundary adopts the finished mesh. The outputs keep running meanwhile.
//
// The rule is by path: an object whose mesh is empty and whose file is neither being
// parsed nor known to fail gets a parse queued (requestMeshLoads), and a finished
// parse fills every such object with that path. So an object that comes back while
// its file is loading, or after (undo of a delete, a duplicated scene), gets its mesh
// too, and two objects on one file parse it once.
//
// An import is not an undo step until its mesh has loaded: then the placeholder is
// swapped for the finished object through AddObjectCommand, at the same place. A
// failed import removes the placeholder, as a failed synchronous import never added
// an object.

#include "EditorState.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "core/UUID.hpp"
#include "scene/Commands.hpp"
#include "scene/MeshObject.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace quadmapper {

// The object's mesh file is being parsed.
inline bool isMeshLoading(const EditorState& es, const MeshObject& object) {
    return object.getMesh().empty() && !object.getSourcePath().empty() &&
           es.meshLoads.tickets.count(object.getSourcePath()) > 0;
}

// The object has a mesh file but no mesh, and none is on its way: it is missing (the
// file is gone or did not parse) and Missing Media offers to relocate it.
inline bool isMeshMissing(const EditorState& es, const MeshObject& object) {
    return !object.getSourcePath().empty() && object.getMesh().empty() && !isMeshLoading(es, object);
}

// Queue a parse for every mesh that is not loaded, not loading and has not failed.
inline void requestMeshLoads(EditorState& es) {
    for (const auto& scene : es.scenes) {
        if (!scene) continue;
        for (const auto& object : scene->getObjects()) {
            if (!object || !object->getMesh().empty()) continue;
            const std::string& path = object->getSourcePath();
            if (path.empty() || es.meshLoads.tickets.count(path) || es.meshLoads.errors.count(path)) {
                continue;
            }
            es.meshLoads.tickets[path] = es.objLoads.request(pathFromUtf8(path));
        }
    }
}

// File > Import 3D Object: a placeholder object in the active scene, selected, while
// its file parses. Returns the placeholder.
inline MeshObject* beginObjectImport(EditorState& es, const std::string& name,
                                     const std::string& absolutePath) {
    Scene& scene = es.activeScene();
    MeshObject* object = scene.createObject(name);
    object->setSourcePath(absolutePath);
    // A new import tries the file again even if it failed before.
    es.meshLoads.errors.erase(absolutePath);
    es.meshLoads.imports.push_back({scene.getId(), object->getId()});
    requestMeshLoads(es);
    return object;
}

// What adoptLoadedMeshes did, for the caller's status line and projector framing:
// every import that finished (ok or not), and every other object whose mesh failed.
struct MeshImportOutcome {
    bool import = true;  // false: an object from the show whose mesh did not load
    bool ok = false;
    UUID sceneId;
    UUID objectId;
    std::string name;
    std::string error;  // !ok
};

// Take the finished parses and put them on their objects. Main thread, at the frame
// boundary, before anything reads the scenes for this frame.
inline std::vector<MeshImportOutcome> adoptLoadedMeshes(EditorState& es) {
    std::vector<MeshImportOutcome> outcomes;
    for (ObjLoadQueue::Completed& done : es.objLoads.takeCompleted()) {
        const std::string path = utf8FromPath(done.path);
        auto ticket = es.meshLoads.tickets.find(path);
        if (ticket == es.meshLoads.tickets.end() || ticket->second != done.ticket) continue;
        es.meshLoads.tickets.erase(ticket);
        if (!done.mesh) es.meshLoads.errors[path] = done.mesh.error().message;

        // Every object waiting for this file.
        std::vector<std::pair<Scene*, MeshObject*>> waiting;
        for (const auto& scene : es.scenes) {
            if (!scene) continue;
            for (const auto& object : scene->getObjects()) {
                if (object && object->getMesh().empty() && object->getSourcePath() == path) {
                    waiting.emplace_back(scene.get(), object.get());
                }
            }
        }

        for (size_t i = 0; i < waiting.size(); ++i) {
            Scene& scene = *waiting[i].first;
            MeshObject& object = *waiting[i].second;
            auto import = std::find_if(es.meshLoads.imports.begin(), es.meshLoads.imports.end(),
                [&](const MeshLoadState::Import& im) {
                    return im.sceneId == scene.getId() && im.objectId == object.getId();
                });
            const bool isImport = import != es.meshLoads.imports.end();
            if (isImport) es.meshLoads.imports.erase(import);

            if (!done.mesh) {
                outcomes.push_back({isImport, false, scene.getId(), object.getId(), object.getName(),
                                    done.mesh.error().message});
                if (isImport) {
                    scene.deleteObject(object.getId());  // as if it had never been added
                } else {
                    Logger::warning("3D object '{}': mesh not loaded ({})", object.getName(),
                                    done.mesh.error().message);
                }
                continue;
            }

            // The last object takes the parsed data; any others get a copy.
            const bool last = i + 1 == waiting.size();
            if (!isImport) {
                if (last) object.setMesh(std::move(done.mesh.value()));
                else object.setMesh(done.mesh.value());
                continue;
            }
            size_t index = 0;
            std::unique_ptr<MeshObject> finished = scene.takeObject(object.getId(), &index);
            if (last) finished->setMesh(std::move(done.mesh.value()));
            else finished->setMesh(done.mesh.value());
            outcomes.push_back({true, true, scene.getId(), finished->getId(), finished->getName(), {}});
            es.commandHistory.execute(std::make_unique<AddObjectCommand>(
                es.quadResolver, scene.getId(), std::move(finished), index));
        }
    }
    // Imports whose placeholder was deleted while loading are ordinary objects if an
    // undo brings them back (requestMeshLoads loads them again).
    es.meshLoads.imports.erase(
        std::remove_if(es.meshLoads.imports.begin(), es.meshLoads.imports.end(),
                       [&](const MeshLoadState::Import& im) {
                           for (const auto& scene : es.scenes) {
                               if (scene && scene->getId() == im.sceneId) {
                                   const MeshObject* o = scene->getObject(im.objectId);
                                   return o == nullptr || !isMeshLoading(es, *o);
                               }
                           }
                           return true;
                       }),
        es.meshLoads.imports.end());
    return outcomes;
}

// The project is being replaced (New, Load, recovery): drop every parse and what is
// known about the old project's mesh files.
inline void cancelMeshLoads(EditorState& es) {
    es.objLoads.cancelAll();
    es.meshLoads = MeshLoadState{};
}

}  // namespace quadmapper
