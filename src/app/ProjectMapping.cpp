#include "ProjectMapping.hpp"
#include "EditorState.hpp"
#include "MediaAssign.hpp"
#include "MeshLoads.hpp"
#include "OutputPersistence.hpp"
#include "ProjectReplace.hpp"
#include "ControlBindings.hpp"
#include "../core/FileSystem.hpp"
#include "../core/Logger.hpp"
#include "../project/Project.hpp"
#include "../scene/Scene.hpp"
#include "../scene/Quad.hpp"
#include "../scene/QuadManager.hpp"
#include "../scene/QuadGroup.hpp"
#include "../scene/MeshObject.hpp"
#include "../media/MediaLibrary.hpp"
#include "../media/MediaItem.hpp"
#include "../source/SourceManager.hpp"
#include "../source/CameraSource.hpp"
#include "../source/HapFileSource.hpp"
#include "../source/StaticImageSource.hpp"
#include "../source/VideoFileSource.hpp"
#include "../source/procedural/ProceduralFactory.hpp"
#include "../output/OutputManager.hpp"
#include "../output/OutputWindow.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace quadmapper {

static std::optional<ProceduralType> proceduralTypeOf(SourceType type) {
    switch (type) {
        case SourceType::PROCEDURAL_SOLID:        return ProceduralType::SOLID_COLOR;
        case SourceType::PROCEDURAL_GRADIENT:     return ProceduralType::GRADIENT;
        case SourceType::PROCEDURAL_CHECKERBOARD: return ProceduralType::CHECKERBOARD;
        case SourceType::PROCEDURAL_TEST_PATTERN: return ProceduralType::TEST_PATTERN;
        case SourceType::PROCEDURAL_NOISE:        return ProceduralType::NOISE;
        case SourceType::PROCEDURAL_PLASMA:       return ProceduralType::PLASMA;
        case SourceType::PROCEDURAL_GRID:         return ProceduralType::GRID;
        default:                                  return std::nullopt;
    }
}

// Describe a live source as a "sources" entry; nullopt for kinds .qmap cannot hold.
static std::optional<Project::SourceData> describeSource(SourceManager& sm, const UUID& id) {
    TextureSource* src = sm.getSource(id);
    if (!src) return std::nullopt;
    Project::SourceData d;
    d.id = id;
    if (auto* image = dynamic_cast<StaticImageSource*>(src)) {
        d.kind = "image";
        d.path = absolutePathString(image->getPath());
    } else if (auto* hap = dynamic_cast<HapFileSource*>(src)) {
        d.kind = "hap";
        d.path = absolutePathString(hap->getPath());
        d.transport = transportOf(*hap);
    } else if (auto* video = dynamic_cast<VideoFileSource*>(src)) {
        d.kind = "video";
        d.path = absolutePathString(video->getPath());
        d.transport = transportOf(*video);
    } else if (auto* camera = dynamic_cast<CameraSource*>(src)) {
        d.kind = "camera";
        d.deviceName = camera->getDeviceName();
    } else if (auto type = proceduralTypeOf(src->getType())) {
        d.kind = "procedural";
        d.proceduralType = ProceduralFactory::getTypeName(*type);
        d.width = src->getWidth();
        d.height = src->getHeight();
    } else {
        return std::nullopt;
    }
    return d;
}

// Recreate a "sources" entry in the SourceManager; returns the live id (null on failure).
static UUID createSourceFrom(SourceManager& sm, const Project::SourceData& d) {
    if (d.kind == "image") return sm.getOrCreateImageSource(pathFromUtf8(d.path));
    if (d.kind == "video" || d.kind == "hap") {
        const UUID id = sm.getOrCreateVideoSource(pathFromUtf8(d.path));  // probes HAP itself
        applySavedTransport(sm, id, d.transport);
        return id;
    }
    if (d.kind == "procedural") {
        for (ProceduralType type : ProceduralFactory::getAvailableTypes()) {
            if (ProceduralFactory::getTypeName(type) == d.proceduralType) {
                return sm.createProceduralSource(type, d.width, d.height);
            }
        }
        Logger::warning("Project: unknown procedural type '{}'", d.proceduralType);
        return UUID();
    }
    if (d.kind == "camera") return sm.createCameraSource(d.deviceName);
    return UUID();
}

void syncEditorToProject(EditorState& es, OutputManager* om, SourceManager* sm,
                         Project& project) {
    project.getScenes().clear();
    project.getOutputs().clear();
    project.getSources().clear();
    std::unordered_set<UUID> describedSources;
    auto describeOnce = [&](const UUID& mediaId) {
        if (mediaId.isNull() || !describedSources.insert(mediaId).second) return;
        // A missing file is written back exactly as it was loaded (PRD §4.1.8).
        if (auto missing = es.missingSources.find(mediaId); missing != es.missingSources.end()) {
            project.getSources().push_back(missing->second);
        } else if (sm) {
            if (auto source = describeSource(*sm, mediaId)) {
                project.getSources().push_back(std::move(*source));
            }
        }
    };

    for (const auto& scenePtr : es.scenes) {
        if (!scenePtr) continue;
        Scene& sc = *scenePtr;
        Project::SceneData sd;
        sd.id = sc.getId();
        sd.name = sc.getName();
        sd.canvas = sc.getCanvasSize();
        sd.canvasLocked = sc.isCanvasLocked();
        const SceneViewState& vs = sc.getViewState();
        sd.view.pan = vs.panOffset;
        sd.view.zoom = vs.zoomLevel;
        sd.view.gridVisible = vs.gridVisible;
        sd.view.snapToGrid = vs.snapToGrid;
        for (const auto& quadPtr : sc.getQuadManager().getQuads()) {
            sd.quads.push_back(*quadPtr);
            auto it = es.quadTexturePaths.find(quadPtr->getId());
            sd.texturePaths.push_back(it != es.quadTexturePaths.end() && !it->second.empty()
                                          ? absolutePathString(it->second) : std::string());
            describeOnce(quadPtr->getTexture().mediaId);
        }
        for (const auto& groupPtr : sc.getGroups()) {
            if (groupPtr) sd.groups.push_back(*groupPtr);
        }
        for (const auto& objectPtr : sc.getObjects()) {
            if (!objectPtr) continue;
            const MeshObject& object = *objectPtr;
            Project::ObjectData od;
            od.id = object.getId();
            od.name = object.getName();
            od.path = object.getSourcePath().empty() ? std::string()
                                                     : absolutePathString(pathFromUtf8(object.getSourcePath()));
            od.transform = object.getTransform();
            od.texture = object.getTexture();
            od.visible = object.isVisible();
            od.opacity = object.getOpacity();
            describeOnce(od.texture.mediaId);
            sd.objects.push_back(std::move(od));
        }
        project.getScenes().push_back(std::move(sd));
    }

    if (om) {
        for (const auto& outPtr : om->getOutputs()) {
            if (!outPtr) continue;
            project.getOutputs().push_back(outputToData(*outPtr, es.scenes));
        }
    }

    project.getMidiMappings().clear();
    for (const auto& mapping : es.midiMappings) {
        project.getMidiMappings().push_back(toProjectMapping(mapping));
    }
}

// Copy a DTO quad's full state onto a freshly created live quad.
static void applyQuadData(const Quad& src, Quad* dst) {
    dst->setVisible(src.isVisible());
    dst->setLocked(src.isLocked());
    dst->setOpacity(src.getOpacity());
    dst->setZOrder(src.getZOrder());
    dst->getGeometry() = src.getGeometry();  // corners + grid + mode + mesh offsets
    dst->getTexture() = src.getTexture();    // UV transform + flip flags
    dst->setColorCorrection(src.getColorCorrection());  // ADR-036
    dst->setSoftEdge(src.getSoftEdge());                // ADR-037
}

static bool isFileSource(const Project::SourceData& source) {
    return source.kind == "image" || source.kind == "video" || source.kind == "hap";
}

void syncProjectToEditor(EditorState& es, OutputManager* om, const Project& project,
                         SourceManager* sm, const MediaLibrary* library) {
    clearProjectContent(es, sm, om, library);  // before any of the new project's sources exist

    // Rebuild outputs from the file (v3), each with the scene it shows (scene ids are
    // kept on load) and its slices. v1/v2 files carry no outputs: keep the existing
    // ones (e.g. the startup default Projector 1) as hardware, but their routing and
    // slices belonged to the show being replaced.
    if (om && !project.getOutputs().empty()) {
        std::vector<UUID> existing;
        for (const auto& o : om->getOutputs()) {
            if (o) existing.push_back(o->getId());
        }
        for (const UUID& id : existing) om->deleteOutput(id);

        for (const auto& od : project.getOutputs()) {
            OutputWindow* ow = om->createOutput(od.name);
            if (!ow) continue;
            applyOutputData(od, *ow);
        }
        // Shown in the Output panel now; F11 and hotplug resolve again (QM-090).
        om->resolveOutputs();
    } else if (om) {
        for (const auto& o : om->getOutputs()) {
            if (!o) continue;
            o->setSceneId(UUID::null());
            o->getSlices().clear();
        }
    }

    // v3.1 sources (ADR-046), created lazily the first time a quad references one:
    // saved source id -> live SourceManager id (null = could not be recreated).
    std::unordered_map<UUID, const Project::SourceData*> savedSources;
    for (const auto& s : project.getSources()) savedSources[s.id] = &s;
    std::unordered_map<UUID, UUID> sourceRemap;
    auto liveSourceFor = [&](const Project::SourceData& src) {
        auto remapIt = sourceRemap.find(src.id);
        if (remapIt == sourceRemap.end()) {
            const UUID live = src.missing ? UUID() : createSourceFrom(*sm, src);
            remapIt = sourceRemap.emplace(src.id, live).first;
        }
        return remapIt->second;
    };

    // A file that could not be opened stays referenced under one missing-source id per
    // path: the saved id, unless a live source (the media bin's) already has it.
    std::unordered_map<std::string, UUID> missingByPath;
    std::unordered_map<UUID, UUID> missingRemap;  // saved source id -> missing-source id
    auto missingIdFor = [&](Project::SourceData src) {
        const UUID savedId = src.id;
        auto it = missingByPath.find(src.path);
        UUID id;
        if (it != missingByPath.end()) {
            id = it->second;
        } else {
            if (src.id.isNull() || sm->hasSource(src.id)) src.id = UUID::generate();
            src.missing = true;
            id = src.id;
            missingByPath.emplace(src.path, id);
            es.missingSources.emplace(id, std::move(src));
        }
        if (!savedId.isNull()) missingRemap.emplace(savedId, id);
        return id;
    };

    std::unordered_map<UUID, UUID> quadRemapAllScenes;  // for the MIDI mapping targets
    for (const auto& sd : project.getScenes()) {
        auto scene = std::make_unique<Scene>(sd.id);
        scene->setName(sd.name);
        scene->setCanvasSize(sd.canvas);
        scene->setCanvasLocked(sd.canvasLocked);
        SceneViewState& vs = scene->getViewState();
        vs.panOffset = sd.view.pan;
        vs.zoomLevel = sd.view.zoom;
        vs.gridVisible = sd.view.gridVisible;
        vs.snapToGrid = sd.view.snapToGrid;

        QuadManager& qm = scene->getQuadManager();
        std::unordered_map<UUID, UUID> quadRemap;  // saved quad id -> live id (new only on a clash)
        for (size_t i = 0; i < sd.quads.size(); ++i) {
            // Keep the saved id so anything addressing a quad by id (OSC, MIDI mappings)
            // still finds it after a reload; mint a fresh one only for a missing or
            // clashing id.
            Quad* nq = nullptr;
            const UUID savedQuadId = sd.quads[i].getId();
            if (!savedQuadId.isNull() && quadRemapAllScenes.count(savedQuadId) == 0) {
                auto adopted = std::make_unique<Quad>(savedQuadId);
                adopted->setName(sd.quads[i].getName());
                nq = qm.insertQuad(std::move(adopted), SIZE_MAX);
            }
            if (!nq) nq = qm.createQuad(sd.quads[i].getName());
            quadRemap[sd.quads[i].getId()] = nq->getId();
            // First wins: a MIDI mapping saved on a duplicated id stays on the quad that
            // kept it, not the copy that had to be renumbered.
            quadRemapAllScenes.emplace(sd.quads[i].getId(), nq->getId());
            applyQuadData(sd.quads[i], nq);

            // The copied binding still carries the SAVED source id; swap it for the
            // live one. The serializer only leaves a mediaId that resolves in the
            // sources table; a source that cannot be opened falls back to texturePath.
            const UUID savedMediaId = nq->getTexture().mediaId;
            nq->getTexture().mediaId = UUID();
            auto savedIt = savedMediaId.isNull() ? savedSources.end()
                                                 : savedSources.find(savedMediaId);
            if (sm && savedIt != savedSources.end()) {
                const Project::SourceData& src = *savedIt->second;
                const UUID live = liveSourceFor(src);
                if (!live.isNull()) {
                    nq->getTexture().mediaId = live;
                    if (!src.path.empty()) es.quadTexturePaths[nq->getId()] = src.path;
                    if (src.kind == "video" || src.kind == "hap") {
                        es.activeVideoSourceId = live;
                    }
                    continue;
                }
            }

            const std::string texturePath =
                i < sd.texturePaths.size() ? sd.texturePaths[i] : std::string();
            if (!texturePath.empty()) {
                // ADR-040 P2: route the media through SourceManager (deduped by
                // path, so the same file on many quads decodes once) and bind the
                // quad by mediaId. quadTexturePaths is kept as the P2 persistence
                // carrier (P5 serializes mediaId/sources directly).
                es.quadTexturePaths[nq->getId()] = texturePath;
                if (sm) {
                    const std::filesystem::path mediaPath = pathFromUtf8(texturePath);
                    const std::string ext = utf8FromPath(mediaPath.extension());
                    const std::optional<MediaType> type = MediaLibrary::getTypeFromExtension(ext);
                    UUID sid;
                    if (type == MediaType::VIDEO) {
                        sid = sm->getOrCreateVideoSource(mediaPath);
                        if (!sid.isNull()) {
                            es.activeVideoSourceId = sid;  // panel controls it
                            // Files older than v3.1 saved no transport state: play
                            // (looping, the default) as on assign, not a paused first frame.
                            if (auto* video = sm->getSourceAs<PlayableSource>(sid)) video->play();
                        }
                    } else if (type == MediaType::IMAGE) {
                        sid = sm->getOrCreateImageSource(mediaPath);
                    }  // any other file is not opened; it is kept as missing below
                    if (!sid.isNull()) nq->getTexture().mediaId = sid;
                }
            }

            // Neither the saved source nor texturePath opened: keep the reference.
            if (!sm || !nq->getTexture().mediaId.isNull()) continue;
            if (savedIt != savedSources.end() && isFileSource(*savedIt->second)) {
                const Project::SourceData& src = *savedIt->second;
                nq->getTexture().mediaId = missingIdFor(src);
                es.quadTexturePaths[nq->getId()] = src.path;
            } else if (!texturePath.empty()) {
                // Pre-v3.1 binding by path alone: saved from now on as a sources entry.
                // A file that is neither image nor video is kept as an image reference
                // for the user to relocate (the sources schema has no "unknown" kind).
                Project::SourceData src;
                src.kind = MediaLibrary::getTypeFromExtension(
                               utf8FromPath(pathFromUtf8(texturePath).extension())) ==
                                   MediaType::VIDEO ? "video" : "image";
                src.path = texturePath;
                nq->getTexture().mediaId = missingIdFor(std::move(src));
            }
        }

        // Re-apply quad groups onto the live scene. createGroup mints fresh ids (and a
        // quad whose saved id clashed got a new one), so member + parent references
        // are remapped off the saved ids.
        std::unordered_map<UUID, UUID> groupRemap;  // saved group id -> fresh live group id
        for (const QuadGroup& sg : sd.groups) {
            QuadGroup* ng = scene->createGroup(sg.getName());
            if (!ng) continue;
            groupRemap[sg.getId()] = ng->getId();
            ng->setVisible(sg.isVisible());
            ng->setLocked(sg.isLocked());
            ng->setCollapsed(sg.isCollapsed());
            for (const UUID& memberId : sg.getMemberQuadIds()) {
                auto it = quadRemap.find(memberId);
                if (it != quadRemap.end()) ng->addMember(it->second);
            }
        }
        // Second pass: rewire nested-group parents once every group exists.
        for (const QuadGroup& sg : sd.groups) {
            if (!sg.getParentGroup().has_value()) continue;
            auto childIt = groupRemap.find(sg.getId());
            auto parentIt = groupRemap.find(*sg.getParentGroup());
            if (childIt != groupRemap.end() && parentIt != groupRemap.end()) {
                if (QuadGroup* live = scene->getGroup(childIt->second)) {
                    live->setParentGroup(parentIt->second);
                }
            }
        }

        // 3D objects (ADR-052): the mesh is re-imported from its file, on a worker
        // (TD-16a, requestMeshLoads below); the media binding is remapped onto the
        // live source like a quad's.
        for (const Project::ObjectData& od : sd.objects) {
            auto object = std::make_unique<MeshObject>(od.id);
            object->setName(od.name);
            object->setSourcePath(od.path);
            object->setTransform(od.transform);
            object->getTexture() = od.texture;
            object->getTexture().mediaId = UUID();
            object->setVisible(od.visible);
            object->setOpacity(od.opacity);

            auto savedIt = od.texture.mediaId.isNull() ? savedSources.end()
                                                       : savedSources.find(od.texture.mediaId);
            if (sm && savedIt != savedSources.end()) {
                const Project::SourceData& src = *savedIt->second;
                const UUID live = liveSourceFor(src);
                if (!live.isNull()) {
                    object->getTexture().mediaId = live;
                    if (src.kind == "video" || src.kind == "hap") es.activeVideoSourceId = live;
                } else if (isFileSource(src)) {
                    object->getTexture().mediaId = missingIdFor(src);
                }
            }

            if (od.path.empty() || od.missing) {
                const std::string why = od.path.empty() ? "no mesh file path saved" : "file not found";
                Logger::warning("3D object '{}': mesh not loaded ({})", od.name, why);
                if (!od.path.empty()) es.meshLoads.errors[od.path] = why;
            }
            scene->adoptObject(std::move(object));
        }

        es.scenes.push_back(std::move(scene));
    }

    // MIDI mappings (ADR-048): sources were just given new live ids, and a quad a new
    // one only if its saved id clashed (scenes keep theirs), so saved targets are
    // rewritten onto them. A target whose source is missing keeps the missing id
    // (relocating it remaps the target, see rebindMissingSource); one whose quad or
    // source is gone keeps its saved id and shows as unresolved.
    es.midiMappings.clear();
    std::unordered_map<UUID, UUID> sourceIds = missingRemap;
    for (const auto& [saved, live] : sourceRemap) {
        if (!live.isNull()) sourceIds[saved] = live;
    }
    for (const auto& data : project.getMidiMappings()) {
        control::MidiMapping mapping = fromProjectMapping(data);
        mapping.target = remapTargetPath(mapping.target, "quad", quadRemapAllScenes);
        mapping.target = remapTargetPath(mapping.target, "source", sourceIds);
        es.midiMappings.push_back(std::move(mapping));
    }

    // Guarantee >= 1 scene, then make the first one active.
    if (es.scenes.empty()) {
        es.activeScene();  // lazily creates a default scene
    }
    // A routed output's slices are in its scene's canvas pixels.
    if (om) {
        for (const auto& o : om->getOutputs()) {
            const Scene* shown = o ? OutputManager::sceneForOutput(es.scenes, *o) : nullptr;
            if (shown) o->setSlicesCanvas(shown->getCanvasSize());
        }
    }
    es.setActiveScene(es.scenes.front()->getId());
    // Start parsing the meshes now, so they count as loading (not missing) at once.
    requestMeshLoads(es);

    // Preserve "launch is aimed at the projector": if outputs exist but no scene is
    // shown on any (e.g. a v1/v2 file), route the active scene to the first output
    // so it still projects on F11.
    if (om && !om->getOutputs().empty()) {
        const bool anyShown = std::any_of(om->getOutputs().begin(), om->getOutputs().end(),
                                          [&](const auto& o) { return o && OutputManager::sceneForOutput(es.scenes, *o); });
        if (!anyShown) {
            om->routeScene(*om->getOutputs().front(), &es.activeScene(), es.scenes);
        }
    }

    // Select the first quad of the active scene (the Inspector's Path field follows it).
    QuadManager& aqm = es.quadManager();
    if (aqm.getQuadCount() > 0) {
        aqm.selectQuad(aqm.getQuads()[0]->getId());
    }
}

}  // namespace quadmapper
