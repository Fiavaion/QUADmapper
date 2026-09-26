#include "Application.hpp"
#include "EditorContext.hpp"
#include "EditorState.hpp"
#include "TextureResolve.hpp"
#include "MediaAssign.hpp"
#include "MissingMedia.hpp"
#include "OutputPersistence.hpp"
#include "ControlBindings.hpp"
#include "../control/ExternalControl.hpp"
#include "../core/Logger.hpp"
#include "../core/FileSystem.hpp"
#include "../core/GlObjectStats.hpp"
#include "../project/Project.hpp"
#include "../project/ProjectSerializer.hpp"
#include "../scene/Scene.hpp"
#include "../scene/Quad.hpp"
#include "../scene/QuadManager.hpp"
#include "../scene/QuadGroup.hpp"
#include "../scene/MeshObject.hpp"
#include "../scene/ObjLoader.hpp"
#include "../scene/ProjectorCamera.hpp"
#include "../media/MediaLibrary.hpp"
#include "../media/MediaItem.hpp"
#include "../media/FolderWatcher.hpp"
#include "../media/ThumbnailCache.hpp"
#include "../source/SourceManager.hpp"
#include "../source/procedural/ProceduralSource.hpp"
#include "../source/CameraSource.hpp"
#include "../source/CameraDevices.hpp"
#include "../source/HapFileSource.hpp"
#include "../source/StaticImageSource.hpp"
#include "../source/VideoFileSource.hpp"
#include "../source/procedural/ProceduralFactory.hpp"
#include "../ai/SidecarClient.hpp"
#include "../renderer/UvTestGrid.hpp"
#include "../renderer/Renderer.hpp"
#include "../renderer/Texture.hpp"
#include "../renderer/GpuCapabilities.hpp"
#include "../output/OutputManager.hpp"
#include "../output/OutputWindow.hpp"
#include "../input/VertexKeyboard.hpp"
#include "../ui/Theme.hpp"
#include "../ui/AsyncFileDialog.hpp"
#include "../ui/panels/OutputPanel.hpp"
#include "../ui/panels/MediaBinPanel.hpp"
#include "../ui/panels/QuadListPanel.hpp"
#include "../ui/dialogs/PreferencesDialog.hpp"
#include "../audio/AudioEngine.hpp"
#include "../audio/AudioMixer.hpp"
#include "SourceAudio.hpp"
#include "../video/VideoPlayer.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <filesystem>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>
#include <unordered_set>
#include <imgui_internal.h>  // For DockBuilder API
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
// NOMINMAX prevents windows.h from defining min/max macros that clash with std::max.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>   // SHGetKnownFolderPath: recovery folder (ADR-046)

namespace quadmapper {

Application* Application::s_instance = nullptr;

// GLFW error callback
static void glfwErrorCallback(int error, const char* description) {
    Logger::error("GLFW Error {}: {}", error, description);
}

// ---- Editor <-> Project persistence bridge (ADR-039 P5) ----
// The live state of truth is EditorState's scene list (Scene/QuadManager) plus the
// OutputManager's output config; the Project DTO is only the serialization shape.
// These two helpers are the single place that copies between them. Save gathers EVERY
// scene + output into the DTO (the serializer then writes .qmap v3); load rebuilds the
// live scenes/outputs and remaps each scene's output assignment onto the freshly
// created output ids. Quad geometry + texture binding (incl. mesh offsets, flipH/V,
// colour, soft edge) copy whole, so nothing is dropped on round-trip. Each bound
// SourceManager source is described into the v3.1 "sources" table (ADR-046).

// Absolute form of a media path; texturePath and file sources are stored absolute.
static std::string absolutePathString(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(path, ec);
    return utf8FromPath(ec ? path : abs);
}

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

static void syncEditorToProject(EditorState& es, OutputManager* om, SourceManager* sm,
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
        sd.assignedOutputId = sc.getAssignedOutputId();
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
            if (outPtr) project.getOutputs().push_back(outputToData(*outPtr));
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

// Media and mesh files that cannot be opened never fail the load (PRD §4.1.8): their
// quads and objects keep the reference (EditorState::missingSources; an object keeps
// its mesh path with an empty mesh) and collectMissingMedia() lists them to relocate.
static void syncProjectToEditor(EditorState& es, OutputManager* om, const Project& project,
                                SourceManager* sm) {
    es.selectedObjectId = UUID();
    es.endCalibration();  // the session's lens and selection belong to the old project
    es.scenes.clear();
    es.quadTextures.clear();
    es.quadTexturePaths.clear();
    clearMissingMedia(es);
    // Release the previous project's media now (the media bin's own generators stay,
    // so its items remain assignable); a reloaded show then starts fresh decoders
    // rather than inheriting ones found by path.
    es.activeVideoSourceId = UUID();
    if (sm) releaseProjectSources(es, *sm);

    // Rebuild outputs from the file (v3) so scene assignments remap onto the freshly
    // created output ids. v1/v2 files carry no outputs — keep the existing ones (e.g.
    // the startup default Projector 1) untouched.
    std::unordered_map<UUID, UUID> outputRemap;  // saved output id -> live output id
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
            outputRemap[od.id] = ow->getId();
        }
        // Shown in the Output panel now; F11 and hotplug resolve again (QM-090).
        om->resolveOutputs();
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
        if (!sd.assignedOutputId.isNull()) {
            auto it = outputRemap.find(sd.assignedOutputId);
            scene->setAssignedOutputId(it != outputRemap.end() ? it->second : UUID::null());
        }
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

        // 3D objects (ADR-052): the mesh is re-imported from its file; the media
        // binding is remapped onto the live source like a quad's.
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

            auto mesh = od.path.empty() ? Err<MeshData>("no mesh file path saved")
                      : od.missing      ? Err<MeshData>("file not found")
                                        : loadObj(pathFromUtf8(od.path));
            if (mesh) {
                object->getMesh() = std::move(mesh.value());
            } else {
                Logger::warning("3D object '{}': mesh not loaded ({})", od.name,
                                mesh.error().message);
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
    es.setActiveScene(es.scenes.front()->getId());

    // Preserve "launch is aimed at the projector": if outputs exist but no scene is
    // assigned to any (e.g. a v1/v2 file), route the active scene to the first output
    // so it still projects on F11.
    if (om && !om->getOutputs().empty()) {
        bool anyAssigned = false;
        for (const auto& scenePtr : es.scenes) {
            if (scenePtr && scenePtr->hasAssignedOutput()) { anyAssigned = true; break; }
        }
        if (!anyAssigned) {
            es.activeScene().setAssignedOutputId(om->getOutputs().front()->getId());
        }
    }

    // Select the first quad of the active scene + sync the texture-path edit buffer.
    QuadManager& aqm = es.quadManager();
    if (aqm.getQuadCount() > 0) {
        aqm.selectQuad(aqm.getQuads()[0]->getId());
        auto pathIt = es.quadTexturePaths.find(aqm.getQuads()[0]->getId());
        if (pathIt != es.quadTexturePaths.end()) {
            strncpy(es.currentTexturePath, pathIt->second.c_str(), sizeof(es.currentTexturePath) - 1);
            es.currentTexturePath[sizeof(es.currentTexturePath) - 1] = '\0';
        } else {
            es.currentTexturePath[0] = '\0';
        }
    }
}

// GLFW window close callback: route the close through the deferred Exit action.
static void windowCloseCallback(GLFWwindow* /*window*/) {
    Application::instance().requestExit();
}

// Editor-window key callback, for the scene take keys only. It is installed before
// the ImGui backend, which chains to it with the untranslated (physical) GLFW key;
// ImGui's own keys are translated through the layout, which moves some top-row digits
// (AZERTY "4" arrives as the apostrophe key), so the take keys cannot use them.
static void editorKeyCallback(GLFWwindow* /*window*/, int key, int /*scancode*/, int action, int mods) {
    const int sceneNumber = OutputManager::sceneTakeNumber(key, action, mods);
    if (sceneNumber > 0) Application::instance().queueSceneTake(sceneNumber);
}

// GLFW framebuffer size callback
static void framebufferSizeCallback(GLFWwindow* /*window*/, int width, int height) {
    glViewport(0, 0, width, height);
}

// GLFW file drop callback for external drag-drop
static void dropCallback(GLFWwindow* /*window*/, int count, const char** paths) {
    Application& app = Application::instance();
    MediaLibrary* library = app.getMediaLibrary();
    if (!library) return;

    int imported = 0;
    for (int i = 0; i < count; i++) {
        const std::filesystem::path filePath = pathFromUtf8(paths[i]);  // GLFW drops are UTF-8
        std::string ext = utf8FromPath(filePath.extension());
        if (MediaLibrary::isExtensionSupported(ext)) {
            if (library->addItem(filePath)) {
                imported++;
            }
        }
    }
    if (imported > 0) {
        Logger::info("Imported {} media file(s) via drag-drop", imported);
    }
}

Application::Application() {
    s_instance = this;
    m_lastSidecarState = ai::SidecarState::Stopped;  // TD-10 watcher seed
}

Application::~Application() {
    shutdown();
    s_instance = nullptr;
}

Application& Application::instance() {
    return *s_instance;
}

bool Application::initialize(int argc, char* argv[]) {
    // Headless harness flags: --stress (QM-081) and --bench (PRD §5 benchmark).
    for (int i = 1; i < argc; ++i) {
        if (!parseStressArg(argv[i], m_stressConfig)) {
            parseBenchArg(argv[i], m_benchConfig);
        }
    }
    if (m_benchConfig) {
        // stdout carries only the JSON report; the log goes to a file beside it.
        Logger::setConsoleOutput(false);
        Logger::enableFileOutput(m_benchConfig->logPath());
    }

    Logger::info("Initializing QUADmapper...");

    m_aiSelfTestPending = (std::getenv("QM_AI_SELFTEST") != nullptr);

    if (!initializeGLFW()) {
        Logger::error("Failed to initialize GLFW");
        return false;
    }

    if (!initializeOpenGL()) {
        Logger::error("Failed to initialize OpenGL");
        return false;
    }

    if (!initializeImGui()) {
        Logger::error("Failed to initialize ImGui");
        return false;
    }

    // Machine preferences, before the components that read them. No file yet (first
    // run) or an unreadable one leaves the defaults.
    m_preferences = std::make_unique<PreferencesDialog>();
    m_preferences->loadPreferences();

    if (!initializeComponents()) {
        Logger::error("Failed to initialize components");
        return false;
    }

    // The stress and bench harnesses measure the render/perform path; they open no
    // ports or devices.
    if (!m_stressConfig && !m_benchConfig) initializeExternalControl();

    Logger::info("QUADmapper initialized successfully");
    m_running = true;
    return true;
}

bool Application::initializeGLFW() {
    Logger::debug("Initializing GLFW...");

    glfwSetErrorCallback(glfwErrorCallback);

    if (!glfwInit()) {
        Logger::error("Failed to initialize GLFW");
        return false;
    }

    // Configure OpenGL 3.3 Core Profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    // Window hints
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_FALSE);

    // Create window
    m_window = glfwCreateWindow(m_windowWidth, m_windowHeight,
                                 m_windowTitle.c_str(), nullptr, nullptr);
    if (!m_window) {
        Logger::error("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }

    // Make OpenGL context current
    glfwMakeContextCurrent(m_window);

    // Enable vsync
    glfwSwapInterval(1);

    // Set callbacks
    glfwSetFramebufferSizeCallback(m_window, framebufferSizeCallback);
    glfwSetDropCallback(m_window, dropCallback);
    glfwSetKeyCallback(m_window, editorKeyCallback);  // before ImGui, which chains to it

    // Get actual framebuffer size (may differ from window size on high-DPI)
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);
    Logger::info("Window created: {}x{}, Framebuffer: {}x{}",
                 m_windowWidth, m_windowHeight, fbWidth, fbHeight);

    return true;
}

bool Application::initializeOpenGL() {
    Logger::debug("Initializing OpenGL with GLAD...");

    // Load OpenGL function pointers with GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        Logger::error("Failed to initialize GLAD");
        return false;
    }

    // Log OpenGL info
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* glslVersion = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));

    Logger::info("OpenGL Vendor: {}", vendor ? vendor : "unknown");
    Logger::info("OpenGL Renderer: {}", renderer ? renderer : "unknown");
    Logger::info("OpenGL Version: {}", version ? version : "unknown");
    Logger::info("GLSL Version: {}", glslVersion ? glslVersion : "unknown");

    // Query BCn texture-compression support once now that a context is current
    // (ADR-044); the HAP upload path gates on this and degrades gracefully.
    GpuCapabilities::get().queryOnce();

    // Set initial viewport
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);
    glViewport(0, 0, fbWidth, fbHeight);

    // Enable depth testing (will be useful later)
    glEnable(GL_DEPTH_TEST);

    // Set clear color (dark gray)
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    return true;
}

bool Application::initializeImGui() {
    Logger::debug("Initializing Dear ImGui...");

    // Check ImGui version
    IMGUI_CHECKVERSION();

    // Create ImGui context
    ImGui::CreateContext();

    // Configure ImGui
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable keyboard navigation
    // A focused panel must not stop the canvas from taking Tab/arrows (the vertex
    // warp keys). Only a widget being edited captures the keyboard; see
    // claimCanvasEditingKeys() below NewFrame().
    io.ConfigNavCaptureKeyboard = false;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;    // Enable multi-viewport

    // Apply QUADmapper "Neon Void" theme
    Theme::apply();

    // Load custom fonts (will fall back to default if not found)
    float dpiScale = 1.0f;
    glfwGetWindowContentScale(m_window, &dpiScale, nullptr);
    Theme::loadFonts(dpiScale);

    // When viewports are enabled, ensure consistent appearance
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        // Maintain our rounded corners in viewport windows
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Initialize platform/renderer backends
    const char* glslVersion = "#version 330 core";
    if (!ImGui_ImplGlfw_InitForOpenGL(m_window, true)) {
        Logger::error("Failed to initialize ImGui GLFW backend");
        return false;
    }

    if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
        Logger::error("Failed to initialize ImGui OpenGL3 backend");
        return false;
    }

    Logger::info("Dear ImGui {} initialized with docking support", IMGUI_VERSION);

    return true;
}

bool Application::initializeComponents() {
    Logger::debug("Initializing application components...");

    // Create project first (other components may depend on it)
    m_project = std::make_unique<Project>();

    // Per-session editor/scene state (replaces render()'s function-statics; see ADR-031 TD-02)
    m_editorState = std::make_unique<EditorState>();

    // Create media library
    m_mediaLibrary = std::make_unique<MediaLibrary>();

    // Create thumbnail cache for media bin
    m_thumbnailCache = std::make_unique<ThumbnailCache>();

    // Create source manager for texture sources (procedural, video, image)
    m_sourceManager = std::make_unique<SourceManager>();

    // Sound (ADR-050 §2): the preferred output device, else the system default. A
    // device that will not open is logged and the app runs silent. The stress harness
    // uses the silent null device so it exercises the audio path without making sound.
    {
        AudioEngine::Options audioOptions;
        audioOptions.nullBackend = m_stressConfig.has_value() || m_benchConfig.has_value();
        m_audioEngine = std::make_unique<AudioEngine>(audioOptions);
        m_audioEngine->open(m_preferences->getPreferences().performance.audioDevice);
    }

    // Optional AI pre-show sidecar (ADR-035 / QM-056). Constructed here but not
    // started — the user enables it on demand from the AI menu. We resolve the
    // sidecar script next to the working dir or the repo root; if it is missing,
    // scriptPath stays empty and start() fails cleanly (AI menu shows disabled).
    {
        ai::SidecarConfig sidecarCfg;
        const std::filesystem::path rel("tools/ai-sidecar/sidecar.py");
        for (const std::filesystem::path& base :
             {std::filesystem::current_path(),
              std::filesystem::current_path() / ".." / "..",
              std::filesystem::current_path() / ".." / ".." / ".."}) {
            std::error_code ec;
            std::filesystem::path candidate = base / rel;
            if (std::filesystem::exists(candidate, ec)) {
                sidecarCfg.scriptPath =
                    utf8FromPath(std::filesystem::weakly_canonical(candidate, ec));
                break;
            }
        }
        m_sidecarClient = std::make_unique<ai::SidecarClient>(sidecarCfg);
    }

    // Auto-load media from default folder if configured
    {
        const AppPreferences& prefs = m_preferences->getPreferences();

        // UI complexity mode (ADR-042): default Simple; persisted choice wins.
        m_editorState->uiMode = (prefs.general.uiMode == 1)
            ? EditorState::UiMode::Advanced : EditorState::UiMode::Simple;

        if (prefs.general.autoLoadMediaOnStartup &&
            !prefs.general.defaultMediaFolder.empty()) {
            const std::filesystem::path mediaFolder = pathFromUtf8(prefs.general.defaultMediaFolder);
            if (std::filesystem::exists(mediaFolder)) {
                int count = m_mediaLibrary->scanFolder(
                    mediaFolder,
                    prefs.general.recursiveFolderScan
                );
                if (count > 0) {
                    Logger::info("Auto-loaded {} media files from {}",
                                count, utf8FromPath(mediaFolder));
                }
            }
        }

        // Set up folder watcher for live file monitoring
        if (!prefs.general.defaultMediaFolder.empty()) {
            m_folderWatcher = std::make_unique<FolderWatcher>();
            m_folderWatcher->setCallback([this](const std::filesystem::path& path, bool isNew) {
                if (isNew) {
                    if (m_mediaLibrary->addItem(path)) {
                        Logger::info("FolderWatcher: Auto-imported new file: {}",
                                    utf8FromPath(path.filename()));
                    }
                } else {
                    // File was modified - could reload texture here in the future
                    Logger::debug("FolderWatcher: File modified: {}",
                                 utf8FromPath(path.filename()));
                }
            });
            m_folderWatcher->watch(
                pathFromUtf8(prefs.general.defaultMediaFolder),
                prefs.general.recursiveFolderScan
            );
        }
    }

    // Create renderer and initialize it
    m_renderer = std::make_unique<Renderer>();
    if (!m_renderer->initialize()) {
        Logger::error("Failed to initialize renderer");
        return false;
    }

    // Create output manager and initialize it
    m_outputManager = std::make_unique<OutputManager>();
    auto outputResult = m_outputManager->initialize(m_window);
    if (!outputResult) {
        Logger::error("Failed to initialize output manager: {}", outputResult.error().message);
        return false;
    }
    m_outputManager->enableMonitorChangeDetection();
    m_outputManager->setNoticeCallback([this](const std::string& message) {
        snprintf(m_editorState->statusMessage, sizeof(m_editorState->statusMessage), "%s",
                 message.c_str());
        m_editorState->statusTimer = 4.0f;
    });
    m_outputManager->setSceneKeyCallback([this](int sceneNumber) { queueSceneTake(sceneNumber); });
    // A removed output must not stay the target of a scene (it would take that
    // scene off the take keys' routing and resurface as a dangling id in the file).
    m_outputManager->setOutputRemovedCallback([this](const UUID& outputId) {
        if (clearOutputAssignments(m_editorState->scenes, outputId)) m_project->setModified(true);
    });
    Logger::info("Output manager initialized with {} monitors", m_outputManager->getMonitorCount());

    // ADR-039 P2: create the default output and route the default scene to it. The
    // default output has no chosen monitor, so it resolves to the first display that
    // is not the editor's (the projector) — "launch already aimed at the projector" —
    // and to none when the editor's screen is the only one (QM-090). There is always
    // >= 1 output, so the F11 lazy-create path becomes a no-op.
    {
        OutputWindow* defaultOutput = m_outputManager->createOutput("Projector 1");
        if (defaultOutput) {
            m_outputManager->resolveOutputs();
            m_editorState->activeScene().setAssignedOutputId(defaultOutput->getId());
            Logger::info("Default scene assigned to output '{}' on monitor {}",
                         defaultOutput->getName(), defaultOutput->getMonitor());
        }
    }

    Logger::debug("All components initialized");
    return true;
}

int Application::run() {
    // QM-081 H2: stress harness runs as a sibling branch, not a hijack of the UI loop.
    if (m_stressConfig) {
        return runStress(*m_stressConfig);
    }
    if (m_benchConfig) {
        return runBench(*m_benchConfig);
    }

    Logger::info("Starting main loop");

    // The close button / Alt+F4 goes through the unsaved-changes check (ADR-046).
    glfwSetWindowCloseCallback(m_window, windowCloseCallback);

    m_lastFrameTime = static_cast<float>(glfwGetTime());

    while (!glfwWindowShouldClose(m_window) && m_running) {
        float deltaTime = 0.0f;
        if (!pollFrame(deltaTime)) break;  // a confirmed Exit
        presentFrame(deltaTime);

        // TD-10: report the outcome of an async sidecar connect (startAsync runs
        // off the UI thread). When the cached state leaves Starting, post a
        // one-shot status so the user sees connected/failed without a freeze.
        if (m_sidecarClient) {
            const ai::SidecarState s = m_sidecarClient->state();
            if (s != m_lastSidecarState) {
                if (m_lastSidecarState == ai::SidecarState::Starting) {
                    EditorState& es = *m_editorState;
                    const bool ok = (s == ai::SidecarState::Attached ||
                                     s == ai::SidecarState::Launched);
                    snprintf(es.statusMessage, sizeof(es.statusMessage), "%s",
                             ok ? "AI assist enabled (sidecar connected)"
                                : "AI assist unavailable - sidecar could not start");
                    es.statusTimer = 5.0f;
                    if (ok) Logger::info(es.statusMessage);
                    else    Logger::warning(es.statusMessage);
                }
                m_lastSidecarState = s;
            }
        }

        // QM-058 verification hook (off unless QM_AI_SELFTEST is set): once the
        // first frames have built the default scene, enable the sidecar, select a
        // quad, and run one "Detect Surface" headlessly so the full chain can be
        // asserted from the log without driving ImGui menus. No-op in production.
        if (m_aiSelfTestPending && ++m_frameCount >= 3) {
            m_aiSelfTestPending = false;
            runAiSelfTest();
        }
    }

    Logger::info("Main loop ended");
    return 0;
}

bool Application::pollFrame(float& deltaTime) {
    // Calculate delta time
    float currentTime = static_cast<float>(glfwGetTime());
    deltaTime = currentTime - m_lastFrameTime;
    m_lastFrameTime = currentTime;

    // Poll events
    glfwPollEvents();

    // Frame boundary: pending file actions (New/Open/Save/Exit, recovery) and
    // autosave run here, before render() takes any scene references (ADR-046).
    runFrameBoundaryTasks();
    return !glfwWindowShouldClose(m_window);
}

void Application::presentFrame(float deltaTime, FrameMarks* marks) {
    using Clock = std::chrono::steady_clock;

    // Update
    update(deltaTime);

    // Render
    render();
    if (marks) marks->rendered = Clock::now();

    // Swap buffers
    glfwSwapBuffers(m_window);
    if (marks) marks->editorSwapped = Clock::now();

    // Show this frame's projector frames on the outputs (after the main frame
    // is committed so we never run inside an ImGui frame).
    if (m_outputManager) m_outputManager->presentOutputs();
    if (marks) marks->outputsPresented = Clock::now();
}

void Application::runAiSelfTest() {
    EditorState& es = *m_editorState;
    if (!es.quadManager().getPrimarySelectedQuad() && !es.quadManager().getQuads().empty()) {
        es.quadManager().selectQuad(es.quadManager().getQuads().front()->getId());
    }
    if (m_sidecarClient) {
        m_sidecarClient->start();
    }

    const char* modeEnv = std::getenv("QM_AI_SELFTEST");
    const std::string mode = modeEnv ? modeEnv : "";
    if (mode == "oneflash") {
        // QM-059: arm the pattern, flash it on the projector, capture a photo.
        oneFlashEmitPattern();
        m_outputManager->handleF11Key();   // enter perform mode -> pattern shows
        oneFlashCapturePhoto();
    } else if (mode == "solve") {
        // QM-060: solve the projector->surface homography and apply the warp.
        oneFlashSolveAndApply();
    } else if (mode == "anamorphic") {
        // QM-062: audience-perspective (frontal-from-viewpoint) warp.
        oneFlashAnamorphic();
    } else {
        aiDetectSurface();
    }
}

void Application::update(float deltaTime) {
    // Update output manager
    if (m_outputManager) {
        // m_outputManager->update(deltaTime);
    }

    (void)deltaTime; // Suppress unused warning for now
    // Note: Video player update is done in render() for now since we need access to static state
}

void Application::render() {
    // Clear the framebuffer
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Per-session editor state now lives in m_editorState (owned member; see ADR-031 TD-02).
    // Each field is aliased to a same-named local reference so the body below is unchanged.
    EditorState& es = *m_editorState;
    using SelectionType = EditorState::SelectionType;
    auto& quadManager = es.quadManager();
    auto& projectPath = es.projectPath;
    auto& statusMessage = es.statusMessage;
    auto& statusTimer = es.statusTimer;
    auto& initialized = es.initialized;

    // Corner/mesh/snap editing state moved into ViewportEditor (ADR-0034). Video is
    // now a SourceManager source updated by updateAll() below (ADR-040 P3) — no manual
    // decode/upload here.

    auto& mediaBinPanel = es.mediaBinPanel;
    auto& mediaBinInitialized = es.mediaBinInitialized;

    auto& outputPanel = es.outputPanel;
    auto& outputPanelInitialized = es.outputPanelInitialized;

    auto& showResources = es.showResources;

    auto& currentSelectionType = es.currentSelectionType;

    // Initialize with one default quad
    if (!initialized) {
        quadManager.createQuad("Quad 1");
        if (quadManager.getQuadCount() > 0) {
            quadManager.selectQuad(quadManager.getQuads()[0]->getId());
        }
        initialized = true;
    }

    // UV test-grid (#2): apply the colored UV reference grid once to any quad we
    // haven't reconciled yet, when the toggle is on. Runs every frame so it
    // covers all creation paths (default, new-project, Quad List "+"). Existing
    // quads get marked seen, so toggling on later only affects future quads.
    for (const auto& qptr : quadManager.getQuads()) {
        const UUID& id = qptr->getId();
        if (es.seenQuadIds.insert(id).second && es.uvGridOnNewQuads) {
            assignUvTestGrid(es.quadTextures, es.quadTexturePaths, id, 1024, 8);
        }
    }

    // Initialize Media Bin Panel
    if (!mediaBinInitialized && m_mediaLibrary) {
        mediaBinPanel.setMediaLibrary(m_mediaLibrary.get());
        mediaBinPanel.setThumbnailCache(m_thumbnailCache.get());
        mediaBinPanel.setSourceManager(m_sourceManager.get());
        mediaBinPanel.setVisible(true);  // Make visible by default
        mediaBinInitialized = true;
    }

    // Initialize Output Panel
    if (!outputPanelInitialized && m_outputManager) {
        outputPanel.setOutputManager(m_outputManager.get());
        outputPanelInitialized = true;
    }

    // Initialize Main Menu Bar callbacks (set once; lambdas capture this). File items
    // queue es.pendingFileAction directly (ADR-046), so they need no callbacks.
    if (!es.mainMenuBarInitialized) {
        // AI pre-show assist (QM-056 skeleton). Active state reads cached client
        // state (no per-frame I/O). Enable launches the sidecar on demand; Detect
        // runs the stub /surface round-trip. Both no-op gracefully if absent.
        es.mainMenuBar.setAiActiveQuery([this]() {
            return m_sidecarClient &&
                   (m_sidecarClient->state() == ai::SidecarState::Attached ||
                    m_sidecarClient->state() == ai::SidecarState::Launched);
        });
        es.mainMenuBar.setOnEnableAi([this]() {
            if (!m_sidecarClient) return;
            // TD-10: connect off the UI thread so the menu never freezes 1-3s.
            // The run loop watches state() and reports the outcome.
            m_sidecarClient->startAsync();
            EditorState& est = *m_editorState;
            snprintf(est.statusMessage, sizeof(est.statusMessage), "%s",
                     "AI: connecting to sidecar...");
            est.statusTimer = 5.0f;
        });
        es.mainMenuBar.setOnDetectSurface([this]() { aiDetectSurface(); });
        es.mainMenuBar.setOnEmitPattern([this]()  { oneFlashEmitPattern();  });
        es.mainMenuBar.setOnCapturePhoto([this]() { oneFlashCapturePhoto(); });
        es.mainMenuBar.setOnClearPattern([this]() { oneFlashClearPattern(); });
        es.mainMenuBar.setOnSolveApply([this]()   { oneFlashSolveAndApply(); });
        es.mainMenuBar.setOnAnamorphic([this]()   { oneFlashAnamorphic(); });
        es.mainMenuBarInitialized = true;
    }

    float deltaTime = ImGui::GetIO().DeltaTime;

    // Update all media sources (ADR-040 P2) and upload any new frames to GPU —
    // main thread / main GL context, once per frame, before any quad is drawn.
    if (m_sourceManager) {
        m_sourceManager->updateAll(static_cast<double>(deltaTime));
        // Each video's sound follows the clock it just advanced (ADR-050 §2).
        syncAudio();
    }

    // Start ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    {
        // The canvas takes Tab/arrows for the selected quad's vertices or, while
        // calibrating the projector it shows, for the calibration points (ADR-052).
        bool canvasKeys = quadManager.getPrimarySelectedQuad() != nullptr;
        if (!es.calibratingOutputId.isNull()) {
            const OutputWindow* calibrated =
                m_outputManager ? m_outputManager->getOutput(es.calibratingOutputId) : nullptr;
            canvasKeys = calibrated &&
                         es.activeScene().getAssignedOutputId() == es.calibratingOutputId &&
                         !calibrated->getCalibrationPoints().empty();
        }
        claimCanvasEditingKeys(canvasKeys);
    }

    // On-canvas viewport: the corner-pin / mesh-edit warp interaction, then every
    // quad drawn to the main GL framebuffer. Runs right after NewFrame(), ahead of
    // every panel, so it acts on this frame's input first (the success-metric
    // vertex-warp UX) and a nudge shows in the very next presented frame (TD-21).
    // See ADR-0034 / ViewportEditor.
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        es.viewportEditor.render(ctx);
    }

    // Create dockspace over the entire viewport
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags dockspaceFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("DockSpace", nullptr, dockspaceFlags);
    ImGui::PopStyleVar(3);

    // Create the dockspace
    ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    // Set up default dock layout on first run
    static bool dockLayoutInitialized = false;
    if (!dockLayoutInitialized) {
        dockLayoutInitialized = true;

        // Clear any existing layout
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        // Split the dockspace into regions
        ImGuiID dockLeft, dockRight, dockCenter, dockBottom;

        // First split: left panel (20% width)
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.20f, &dockLeft, &dockCenter);

        // Second split: right panel from center (25% of remaining)
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.30f, &dockRight, &dockCenter);

        // Third split: bottom panel from center (25% height)
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.25f, &dockBottom, &dockCenter);

        // Dock windows to their respective nodes
        ImGui::DockBuilderDockWindow("Quad List", dockLeft);
        ImGui::DockBuilderDockWindow("Video Player", dockLeft);  // Tabbed with Quad List
        ImGui::DockBuilderDockWindow("Properties", dockRight);
        ImGui::DockBuilderDockWindow("Calibration", dockRight);  // Tabbed with Properties
        ImGui::DockBuilderDockWindow("Control", dockRight);      // Tabbed with Properties (ADR-048)
        ImGui::DockBuilderDockWindow("Resources", dockBottom);  // Tabbed panel with Project, Media, Outputs

        // Finish the layout
        ImGui::DockBuilderFinish(dockspaceId);
    }

    // Capture the central node rect (the transparent canvas region between panels)
    // for next frame's ViewportEditor view transform (ADR-041). Convert from ImGui
    // screen space to window-local coords so it matches glfwGetCursorPos.
    if (ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId)) {
        const ImVec2 vpPos = viewport->Pos;
        es.canvasRegionPos = Vec2(centralNode->Pos.x - vpPos.x,
                                  centralNode->Pos.y - vpPos.y);
        es.canvasRegionSize = Vec2(centralNode->Size.x, centralNode->Size.y);
    }

    ImGui::End();

    ImGuiIO& io = ImGui::GetIO();

    // Main menu bar
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        ctx.audio = m_audioEngine.get();
        es.mainMenuBar.render(ctx);
        saveAudioDevicePreference();
    }
    saveUiModePreference();

    // Viewport zoom/pan overlay + input (ADR-041). The central node is a passthrough
    // hole, so the mouse is over the canvas (not a panel) exactly when it is in the
    // region and no ImGui window has capture. Mutates canvasZoom/canvasPan, which the
    // next frame's ViewportEditor consumes; anchored zoom reuses last frame's view.
    if (es.canvasRegionSize.x > 1.0f && es.canvasRegionSize.y > 1.0f) {
        const ImVec2 vpPos = ImGui::GetMainViewport()->Pos;
        const ImVec2 regionAbs(es.canvasRegionPos.x + vpPos.x,
                               es.canvasRegionPos.y + vpPos.y);
        const ImVec2 regionSz(es.canvasRegionSize.x, es.canvasRegionSize.y);
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool overRegion =
            mouse.x >= regionAbs.x && mouse.x <= regionAbs.x + regionSz.x &&
            mouse.y >= regionAbs.y && mouse.y <= regionAbs.y + regionSz.y;

        // Scroll-wheel zoom, anchored on the cursor (uses last frame's transform).
        if (overRegion && !io.WantCaptureMouse && io.MouseWheel != 0.0f &&
            es.lastViewScale.x != 0.0f && es.lastViewScale.y != 0.0f) {
            const float oldZoom = es.canvasZoom;
            float newZoom = oldZoom * (1.0f + 0.12f * io.MouseWheel);
            newZoom = std::min(12.0f, std::max(0.1f, newZoom));
            const float cx = (mouse.x - regionAbs.x) / regionSz.x;
            const float cy = (mouse.y - regionAbs.y) / regionSz.y;
            const float px = (cx - es.lastViewOffset.x) / es.lastViewScale.x;
            const float py = (cy - es.lastViewOffset.y) / es.lastViewScale.y;
            const float nsx = es.lastViewScale.x * (newZoom / oldZoom);
            const float nsy = es.lastViewScale.y * (newZoom / oldZoom);
            es.canvasPan.x = (cx - px * nsx) - (1.0f - nsx) * 0.5f;
            es.canvasPan.y = (cy - py * nsy) - (1.0f - nsy) * 0.5f;
            es.canvasZoom = newZoom;
        }

        // Middle-drag pan (per-frame delta in region-normalized units).
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            es.canvasPan.x += d.x / regionSz.x;
            es.canvasPan.y += d.y / regionSz.y;
        }

        // Compact controls overlay in the top-left corner of the canvas region.
        ImGui::SetNextWindowPos(ImVec2(regionAbs.x + 8.0f, regionAbs.y + 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f);
        const ImGuiWindowFlags overlayFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("##ViewportZoom", nullptr, overlayFlags)) {
            if (ImGui::SmallButton("Fit")) { es.canvasZoom = 1.0f; es.canvasPan = Vec2(0.0f, 0.0f); }
            ImGui::SameLine();
            if (ImGui::SmallButton("-")) es.canvasZoom = std::max(0.1f, es.canvasZoom / 1.12f);
            ImGui::SameLine();
            ImGui::Text("%3.0f%%", es.canvasZoom * 100.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("+")) es.canvasZoom = std::min(12.0f, es.canvasZoom * 1.12f);
        }
        ImGui::End();

        // Drop media onto the quad under the cursor (ADR-042). A custom ImGui drop
        // target can't register over the passthrough central node (no window is hovered
        // there), so detect the drop manually: while a MEDIA_ITEM_ID drag is active,
        // on left-release over the canvas, assign to the topmost quad under the cursor.
        const ImGuiPayload* drag = ImGui::GetDragDropPayload();
        if (drag && drag->IsDataType("MEDIA_ITEM_ID") &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left) && overRegion &&
            es.lastViewScale.x != 0.0f && es.lastViewScale.y != 0.0f) {
            UUID mediaId = *(const UUID*)drag->Data;
            MediaItem* item = m_mediaLibrary ? m_mediaLibrary->getItem(mediaId) : nullptr;
            if (item) {
                const float cx = (mouse.x - regionAbs.x) / regionSz.x;
                const float cy = (mouse.y - regionAbs.y) / regionSz.y;
                const Vec2 canvasPt((cx - es.lastViewOffset.x) / es.lastViewScale.x,
                                    (cy - es.lastViewOffset.y) / es.lastViewScale.y);
                if (Quad* target = es.quadManager().quadAtPoint(canvasPt)) {
                    UUID sid = assignMediaToQuad(es, m_sourceManager.get(),
                                                 m_project.get(), *target, *item);
                    if (!sid.isNull()) {
                        es.quadManager().selectQuad(target->getId());
                        snprintf(es.statusMessage, sizeof(es.statusMessage),
                                 "Assigned '%s' to %s", item->getName().c_str(),
                                 target->getName().c_str());
                        es.statusTimer = 3.0f;
                    }
                }
            }
        }
    }

    // Handle keyboard shortcuts. Editing shortcuts stay off while text is typed or a
    // popup/modal is open (e.g. the unsaved-changes prompt: Delete must not delete
    // quads behind it, Ctrl+S must not bypass it). F11/Esc are handled separately.
    const bool editingKeys = !io.WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // While calibrating a projector the quads are hidden and not being edited, so the
    // quad shortcuts (undo, select all, z-order, mesh) must not reach them.
    const bool quadKeys = editingKeys && es.calibratingOutputId.isNull();
    if (editingKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        saveProject();
    }

    if (quadKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (es.commandHistory.undo()) m_project->setModified(true);
    }
    if (quadKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        if (es.commandHistory.redo()) m_project->setModified(true);
    }

    if (quadKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        quadManager.selectAll();
    }

    // Edit > Preferences. Ctrl+, is no other shortcut's key.
    if (editingKeys && io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
        ImGui::IsKeyPressed(ImGuiKey_Comma, false)) {
        es.showPreferences = true;
    }

    if (editingKeys && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        if (!es.calibratingOutputId.isNull()) {
            // Calibrating: Delete removes the selected calibration point, never quads.
            OutputWindow* calibrated = m_outputManager->getOutput(es.calibratingOutputId);
            if (calibrated && es.calibration.selected >= 0) {
                es.calibration.removePoint(calibrated->getCalibrationPoints(),
                                           es.calibration.selected);
                m_project->setModified(true);
            }
        } else if (quadManager.hasSelection()) {
            es.deleteSelectedQuads();
            m_project->setModified(true);
        }
    }

    // Scene take (US-008): the 1-9 presses queued since the last frame put scene N on
    // air. Not while a text field is active (renaming a scene, typing a number) or a
    // popup is open; presses made then are dropped, never replayed later.
    if (editingKeys) {
        for (int n : m_pendingSceneTakes) takeSceneByNumber(n);
    }
    m_pendingSceneTakes.clear();
    // OSC/MIDI takes (ADR-048) go through the same take but skip that guard: it exists
    // so keys typed into a field or behind a modal do not act on the show, and an
    // operator's controller is not typing. Calibration still holds them off
    // (takeSceneByNumber), as it does the keys.
    for (int n : m_pendingExternalSceneTakes) {
        takeSceneByNumber(n);
        Logger::info("Control: scene take {} requested; active scene is now '{}'", n,
                     es.activeScene().getName());
    }
    m_pendingExternalSceneTakes.clear();

    // Blackout (ADR-048 §3): Ctrl+B, the panic key, works with a popup open like F11;
    // only a text field keeps it. Tab/arrows/Delete/1-9/Ctrl+S/Z/Y/A/M/[/] and Alt+=/-/R
    // are the editing keys, so Ctrl+B collides with none of them.
    if (!io.WantTextInput && io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper &&
        ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        m_outputManager->toggleBlackout();
    }

    // F11 toggles perform mode and Esc leaves it, even with a popup open: the way out
    // of fullscreen must never take two presses. Only a text field keeps them (its Esc
    // cancels the edit).
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
        m_outputManager->handleF11Key();
    }
    // While calibrating, the editor's Esc belongs to calibration: it cancels a pick in
    // progress, else leaves calibration, and never also leaves perform mode (Esc on an
    // output window still does).
    // Esc also cancels a pending MIDI learn, in the same press.
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (m_externalControl) m_externalControl->dispatcher().cancelLearn();
        if (es.calibratingOutputId.isNull()) {
            m_outputManager->handleEscapeKey();
        } else if (es.calibration.pick != CalibrationSession::Pick::None) {
            es.calibration.cancelPick();
        } else {
            es.endCalibration();
        }
    }

    // Z-order and mesh keyboard shortcuts
    if (quadKeys) {
        // Ctrl+] : Move selected quad(s) up (bring forward - render later = on top)
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedUp();
                m_project->setModified(true);
            }
        }

        // Ctrl+[ : Move selected quad(s) down (send backward - render earlier = behind)
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedDown();
                m_project->setModified(true);
            }
        }

        // Ctrl+Shift+] : Move to top (bring to front)
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedToTop();
                m_project->setModified(true);
            }
        }

        // Ctrl+Shift+[ : Move to bottom (send to back)
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedToBottom();
                m_project->setModified(true);
            }
        }


        // Mesh editing shortcuts
        Quad* meshQuad = quadManager.getPrimarySelectedQuad();
        if (meshQuad) {
            QuadGeometry& geo = meshQuad->getGeometry();

            // Ctrl+M : Toggle mesh edit mode
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
                bool isEdit = (geo.getMeshMode() == MeshMode::MESH_EDIT);
                geo.setMeshMode(isEdit ? MeshMode::CORNERS_ONLY : MeshMode::MESH_EDIT);
                m_project->setModified(true);
            }

            // Alt+= : Increase mesh resolution
            if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Equal, false)) {
                int res = geo.getMeshResolution();
                if (res < 64) {
                    geo.setMeshResolution(res + 1);
                    m_project->setModified(true);
                }
            }

            // Alt+- : Decrease mesh resolution
            if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Minus, false)) {
                int res = geo.getMeshResolution();
                if (res > 2) {
                    geo.setMeshResolution(res - 1);
                    m_project->setModified(true);
                }
            }

            // Alt+R : Reset mesh to bilinear
            if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                if (geo.getMeshMode() == MeshMode::MESH_EDIT && geo.hasMeshModifications()) {
                    geo.resetMesh();
                    m_project->setModified(true);
                }
            }
        }
    }

    // ============ Quad List Panel ============
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        es.quadListPanel.render(ctx);
    }

    // ============ Properties Panel (Context-Sensitive) ============
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        es.inspectorPanel.render(ctx);
    }

    // ============ Calibration Panel (only while calibrating, ADR-052) ============
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        es.calibrationPanel.render(ctx);
        es.calibrationPanel.drawCanvasOverlay(ctx);
    }

    // ============ Resources Panel (Tabbed: Project, Media, Outputs) ============
    if (showResources) {
    ImGui::Begin("Resources");

    if (ImGui::BeginTabBar("ResourcesTabs")) {
        // Simple mode (ADR-042) shows only the Media bin; Project/Scenes/Preview/
        // Outputs are Advanced-only (single-projector output is auto + the F11 button).
        const bool advanced = (es.uiMode == EditorState::UiMode::Advanced);

        // -------- Project Tab --------
        if (advanced && ImGui::BeginTabItem("Project")) {
            ImGui::Text("Project: %s", m_project->getName().c_str());
            if (m_project->isModified()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "(modified)");
            }
            ImGui::Separator();

            ImGui::Text("File Path:");
            ImGui::InputText("##projectpath", projectPath, sizeof(projectPath));
            ImGui::SameLine();
            if (ImGui::Button("Save")) {
                saveProject();
            }
            ImGui::SameLine();
            if (ImGui::Button("Load")) {
                loadProject();
            }

            // Status message display
            if (statusTimer > 0.0f) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", statusMessage);
                statusTimer -= ImGui::GetIO().DeltaTime;
            }

            ImGui::EndTabItem();
        }

        // -------- Media Bin Tab --------
        if (ImGui::BeginTabItem("Media")) {
            UUID prevMediaSelection = mediaBinPanel.getSelectedItemId();
            mediaBinPanel.renderContent();
            // Switch to Media properties if a media item was just selected
            if (mediaBinPanel.hasSelection() && mediaBinPanel.getSelectedItemId() != prevMediaSelection) {
                currentSelectionType = SelectionType::Media;
                quadManager.deselectAll();
                outputPanel.clearSelection();
            }
            ImGui::EndTabItem();
        }

        // -------- Scenes Tab --------
        if (advanced && ImGui::BeginTabItem("Scenes")) {
            EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                              m_sourceManager.get(), m_outputManager.get(),
                              m_renderer.get(), m_window};
            es.scenePanel.renderContent(ctx);
            ImGui::EndTabItem();
        }

        // -------- Preview Tab --------
        if (advanced && ImGui::BeginTabItem("Preview")) {
            EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                              m_sourceManager.get(), m_outputManager.get(),
                              m_renderer.get(), m_window};
            es.outputPreviewPanel.renderContent(ctx);
            ImGui::EndTabItem();
        }

        // -------- Outputs Tab --------
        if (advanced && ImGui::BeginTabItem("Outputs")) {
            UUID prevOutputSelection = outputPanel.getSelectedOutputId();
            outputPanel.renderContent();
            // Switch to Output properties if an output was just selected
            if (outputPanel.hasSelection() && outputPanel.getSelectedOutputId() != prevOutputSelection) {
                currentSelectionType = SelectionType::Output;
                quadManager.deselectAll();
                mediaBinPanel.clearSelection();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
    } // end showResources

    // ============ Video Player Panel ============
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        ctx.audio = m_audioEngine.get();
        es.videoPlayerPanel.render(ctx);
    }

    // ============ Control Panel (OSC / MIDI, ADR-048) ============
    if (m_externalControl) {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        if (es.controlPanel.render(ctx, *m_externalControl, m_preferences->getPreferences().control,
                                   glfwGetTime())) {
            saveControlPreferences();
        }
    }

    // ============ Preferences (Edit > Preferences..., Ctrl+,) ============
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        ctx.audio = m_audioEngine.get();
        m_preferences->render(ctx, m_externalControl.get());
        saveAudioDevicePreference();  // a device picked in the Audio tab
        saveUiModePreference();       // a mode picked in the General tab
    }

    if (m_outputManager && m_outputManager->isBlackout()) drawBlackoutBanner();

    // Demo window for testing ImGui (can be removed later)
    static bool showDemo = false;
    if (showDemo) {
        ImGui::ShowDemoWindow(&showDemo);
    }

    renderMissingMediaDialog();
    // Unsaved-changes / recovery modals (ADR-046); drawn last so they sit on top.
    renderSessionDialogs();

    if (m_benchInFrameEdit) m_benchInFrameEdit();

    // Draw each projector's frame once (ADR-054), after every edit this frame made:
    // the canvas, the shortcuts, scene takes (keys 1-9, OSC/MIDI), blackout and every
    // panel, so each reaches the projector in the frame it was made (PRD: scene switch
    // < 16 ms). The output windows copy these after the editor's swap; the Preview tab
    // recorded the same textures above and ImGui samples them below, so it shows this
    // frame's image (a resize keeps the texture names, see Framebuffer::resize).
    renderProjectorFrames();

    // Finalize ImGui rendering
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Handle multi-viewport platform windows
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backupContext = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backupContext);
    }
}

// ---- Project lifecycle (TD-02, ADR-046) ----
// Everything that replaces or writes the project runs here, at a frame boundary
// (runFrameBoundaryTasks, called from pollFrame() before presentFrame() renders),
// never from inside the ImGui frame: File > Load used to destroy the Scene whose
// QuadManager& render() was still using. Callers only queue an EditorState::FileAction.

namespace {

namespace fs = std::filesystem;

// %LOCALAPPDATA%\QUADmapper\recovery: autosave / crash files of untitled projects.
fs::path recoveryDir() {
    PWSTR base = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = fs::path(base) / "QUADmapper" / "recovery";
    }
    CoTaskMemFree(base);
    if (dir.empty()) dir = fs::temp_directory_path() / "QUADmapper" / "recovery";
    return dir;
}

// <project>.autosave.qmap / <project>.crash.qmap beside the project, or
// untitled.<kind>.qmap in the recovery folder. kind = "autosave" | "crash".
fs::path recoveryFileFor(const std::string& projectPath, const char* kind) {
    const std::string suffix = std::string(".") + kind + ".qmap";
    if (projectPath.empty()) return recoveryDir() / ("untitled" + suffix);
    fs::path p = pathFromUtf8(projectPath);
    return p.replace_extension(suffix);
}

// Delete a project's autosave + crash files (and the .bak the atomic write keeps) once
// its state is settled. While a recovery prompt is unanswered nothing is deleted, so
// closing the window (or saving) before answering it cannot lose that work.
void discardRecoveryFiles(const EditorState& es, const std::string& projectPath) {
    if (!es.recoveryFilePath.empty()) return;
    std::error_code ec;
    for (const char* kind : {"autosave", "crash"}) {
        const fs::path file = recoveryFileFor(projectPath, kind);
        fs::remove(file, ec);
        fs::remove(fs::path(file) += ".bak", ec);
    }
}

// Native Open/Save dialog request for .qmap files (shown by AsyncFileDialog, off the
// render thread).
AsyncFileDialog::Request projectFileRequest(bool save, const char* initialPath) {
    static const char filter[] = "QUADmapper Project (*.qmap)\0*.qmap\0All Files (*.*)\0*.*\0";
    AsyncFileDialog::Request request;
    request.kind = save ? AsyncFileDialog::Kind::SaveFile : AsyncFileDialog::Kind::OpenFile;
    request.title = save ? "Save Project" : "Open Project";
    request.filter.assign(filter, sizeof(filter));
    request.defaultExt = "qmap";
    request.initialPath = initialPath;
    return request;
}

void postStatus(EditorState& es, const std::string& message) {
    snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", message.c_str());
    es.statusTimer = 4.0f;
}

// Appended to the load status when media or mesh files are missing.
std::string missingMediaNote(size_t missing) {
    if (missing == 0) return std::string();
    return " (warning: " + std::to_string(missing) + " media file" +
           (missing == 1 ? "" : "s") + " missing)";
}

// Native file picker for relocating one missing file, pre-filled with its name (and
// opening in its old folder when that still exists).
AsyncFileDialog::Request locateFileRequest(const MissingMediaItem& item) {
    static const char meshFilter[] = "Wavefront OBJ (*.obj)\0*.obj\0All Files (*.*)\0*.*\0";
    static const char mediaFilter[] = "All Files (*.*)\0*.*\0";
    const fs::path original = pathFromUtf8(item.path);
    AsyncFileDialog::Request request;
    request.kind = AsyncFileDialog::Kind::OpenFile;
    request.title = "Locate " + utf8FromPath(original.filename());
    if (item.kind == MissingMediaItem::Kind::Mesh) request.filter.assign(meshFilter, sizeof(meshFilter));
    else                                           request.filter.assign(mediaFilter, sizeof(mediaFilter));
    std::error_code ec;
    request.initialPath = fs::is_directory(original.parent_path(), ec) ? utf8FromPath(original)
                                                                        : utf8FromPath(original.filename());
    return request;
}

AsyncFileDialog::Request searchFolderRequest() {
    AsyncFileDialog::Request request;
    request.kind = AsyncFileDialog::Kind::PickFolder;
    request.title = "Search this folder (and its subfolders) for the missing files";
    return request;
}

AsyncFileDialog::Request objectFileRequest() {
    static const char filter[] = "Wavefront OBJ (*.obj)\0*.obj\0All Files (*.*)\0*.*\0";
    AsyncFileDialog::Request request;
    request.kind = AsyncFileDialog::Kind::OpenFile;
    request.title = "Import 3D Object";
    request.filter.assign(filter, sizeof(filter));
    return request;
}

// File > Import 3D Object (ADR-052): the object goes into the active scene. An
// uncalibrated projector on that scene's output is re-aimed to frame it, so the
// object is in view as soon as it is imported.
void importObjectFile(EditorState& es, OutputManager* om, Project* project,
                      const std::string& path) {
    const std::filesystem::path file = pathFromUtf8(path);
    auto mesh = loadObj(file);
    if (!mesh) {
        Logger::error("Import 3D object failed: {}", mesh.error().message);
        postStatus(es, "Import failed: " + mesh.error().message);
        return;
    }
    Scene& scene = es.activeScene();
    MeshObject* object = scene.createObject(utf8FromPath(file.stem()));
    object->setSourcePath(absolutePathString(file));
    object->getMesh() = std::move(mesh.value());

    OutputWindow* output = (om && scene.hasAssignedOutput())
                               ? om->getOutput(scene.getAssignedOutputId()) : nullptr;
    if (output && !output->getProjector().calibrated) {
        Vec3 boxMin, boxMax;
        object->worldBounds(boxMin, boxMax);
        const IVec2 res = output->getEffectiveResolution();
        output->getProjector() = ProjectorCamera::framing(
            boxMin, boxMax, ProjectorIntrinsics::fromThrowRatio(1.5f, res.x, res.y));
    }

    es.selectedObjectId = object->getId();
    scene.getQuadManager().deselectAll();  // Delete/arrows must not reach a hidden quad
    es.currentSelectionType = EditorState::SelectionType::Object;
    es.mediaBinPanel.clearSelection();
    es.outputPanel.clearSelection();
    if (project) project->setModified(true);
    postStatus(es, "Imported 3D object: " + object->getName() + " (" +
                   std::to_string(object->getMesh().triangleCount()) + " triangles)");
}

}  // namespace

void Application::takeSceneByNumber(int number) {
    EditorState& es = *m_editorState;
    // Calibration edits the scene routed to the calibrated output; keep it there.
    if (!es.calibratingOutputId.isNull()) return;

    std::vector<SceneTakeOutput> outputs;
    for (const auto& output : m_outputManager->getOutputs()) {
        if (output) outputs.push_back({output->getId(), output->isEnabled()});
    }
    const SceneTake take = es.takeScene(number, outputs);
    if (take.kind == SceneTake::Kind::None) return;
    // Takes run inside render(), after the frame-boundary fit and before this frame's
    // projector frames are drawn: re-fit now so those frames are framed for the scene
    // just put on air.
    fitProjectorsToOutputs();

    const Scene& scene = es.activeScene();
    if (take.kind == SceneTake::Kind::Route) {
        m_project->setModified(true);
        const OutputWindow* output = m_outputManager->getOutput(take.outputId);
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Scene %d: %s on %s", number,
                 scene.getName().c_str(), output ? output->getName().c_str() : "output");
    } else {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Scene %d: %s", number,
                 scene.getName().c_str());
    }
    es.statusTimer = 3.0f;
}

void Application::initializeExternalControl() {
    m_externalControl = std::make_unique<control::ExternalControl>();

    ControlBindingHost host;
    host.scenes = &m_editorState->scenes;
    host.sources = m_sourceManager.get();
    host.takeScene = [this](int number) { m_pendingExternalSceneTakes.push_back(number); };
    host.activeSceneNumber = [this]() {
        const EditorState& es = *m_editorState;
        for (size_t i = 0; i < es.scenes.size(); ++i) {
            if (es.scenes[i] && es.scenes[i]->getId() == es.activeSceneId) return static_cast<int>(i) + 1;
        }
        return 0;
    };
    host.isBlackout = [this]() { return m_outputManager && m_outputManager->isBlackout(); };
    host.setBlackout = [this](bool on) { if (m_outputManager) m_outputManager->setBlackout(on); };
    host.refuse = [this](const std::string& address, const std::string& reason) {
        m_externalControl->dispatcher().noteRefused(address, reason);
    };
    host.getMasterVolume = [this]() { return m_audioEngine ? m_audioEngine->masterVolume() : 1.0f; };
    host.setMasterVolume = [this](float v) { if (m_audioEngine) m_audioEngine->setMasterVolume(v); };
    host.isMasterMuted = [this]() { return m_audioEngine && m_audioEngine->masterMuted(); };
    host.setMasterMuted = [this](bool on) { if (m_audioEngine) m_audioEngine->setMasterMuted(on); };
    registerControlTargets(m_externalControl->registry(), host);

    const ControlPreferences& prefs = m_preferences->getPreferences().control;
    m_externalControl->setWantedMidiDevices(prefs.midiDevices);
    m_externalControl->setFeedback(ControlSettingsEditor::feedbackSettings(prefs));
    if (prefs.oscEnabled && !m_externalControl->startOsc(prefs.oscBindAddress, prefs.oscPort)) {
        postStatus(*m_editorState, "OSC input is off: " + m_externalControl->oscError());
    }
}

void Application::processExternalControl() {
    if (!m_externalControl) return;
    EditorState& es = *m_editorState;
    const double now = glfwGetTime();
    m_externalControl->maintainMidi(now);
    // Names and ids can change with any edit; re-read them when something will use them.
    if (m_externalControl->hasPendingEvents() || es.showControl || m_externalControl->feedbackDue(now)) {
        refreshControlEntities(m_externalControl->registry(), es.scenes, m_sourceManager.get());
    }
    m_externalControl->processEvents(es.midiMappings, now);
    m_externalControl->updateFeedback(now);
}

void Application::saveControlPreferences() {
    m_preferences->savePreferences();
}

void Application::saveUiModePreference() {
    const int mode = m_editorState->uiMode == EditorState::UiMode::Advanced ? 1 : 0;
    int& saved = m_preferences->getPreferences().general.uiMode;
    if (mode == saved) return;
    saved = mode;
    m_preferences->savePreferences();
}

void Application::drawBlackoutBanner() {
    const EditorState& es = *m_editorState;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool haveCanvas = es.canvasRegionSize.x > 1.0f && es.canvasRegionSize.y > 1.0f;
    // Across the top of the canvas, just below the zoom controls so they stay usable.
    const float top = haveCanvas ? es.canvasRegionPos.y : viewport->WorkPos.y - viewport->Pos.y;
    const ImVec2 origin(viewport->Pos.x + (haveCanvas ? es.canvasRegionPos.x : 0.0f),
                        viewport->Pos.y + top + ImGui::GetFrameHeight() + 16.0f);
    const float width = haveCanvas ? es.canvasRegionSize.x : viewport->WorkSize.x;

    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 2.2f;
    const char* title = "BLACKOUT";
    const char* detail = "every projector output is black  -  Ctrl+B or Output > Blackout to restore";
    const ImVec2 titleSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, title);
    const ImVec2 detailSize = ImGui::CalcTextSize(detail);
    const float height = titleSize.y + detailSize.y + 16.0f;

    ImDrawList* draw = ImGui::GetForegroundDrawList(viewport);
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(200, 20, 20, 235));
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
    draw->AddText(font, size, ImVec2(origin.x + (width - titleSize.x) * 0.5f, origin.y + 4.0f),
                  IM_COL32(255, 255, 255, 255), title);
    draw->AddText(ImVec2(origin.x + (width - detailSize.x) * 0.5f, origin.y + 8.0f + titleSize.y),
                  IM_COL32(255, 255, 255, 255), detail);
}

void Application::saveProject() {
    m_editorState->pendingFileAction = EditorState::FileAction::Save;
}

void Application::loadProject() {
    m_editorState->pendingLoadPath = m_editorState->projectPath;
    m_editorState->pendingFileAction = EditorState::FileAction::Load;
}

void Application::requestExit() {
    if (m_exitConfirmed || !m_editorState) return;  // our own confirmed close: let it through
    glfwSetWindowShouldClose(m_window, GLFW_FALSE);
    m_editorState->pendingFileAction = EditorState::FileAction::Exit;
}

void Application::runFrameBoundaryTasks() {
    // The clean baseline is the editor after its first frame (the default quad
    // exists by then); recovery is offered from the same point.
    if (!m_haveCleanBaseline) {
        if (!m_editorState->initialized) return;
        markProjectClean();
        m_haveCleanBaseline = true;
        offerRecovery("");
    }
    processPendingFileAction();
    processExternalControl();
    pollMissingMediaSearch();
    if (m_sourceManager) releaseUnusedSources(*m_editorState, *m_sourceManager);
    fitProjectorsToOutputs();
    autosaveIfDue();
}

// Keep each output's projector camera matched to what it shows now (ADR-052): its
// current rotation-aware size and, while uncalibrated, framed on its scene's objects.
// Every render and calibration path then reads a camera that is already right.
void Application::fitProjectorsToOutputs() {
    if (!m_outputManager) return;
    for (const auto& output : m_outputManager->getOutputs()) {
        if (!output) continue;
        const IVec2 size = output->getEffectiveResolution();
        const Scene* scene = OutputManager::sceneForOutput(m_editorState->scenes, output->getId());
        Vec3 lo, hi;
        const bool framed = scene && scene->visibleObjectBounds(lo, hi);
        output->getProjector() = ProjectorCamera::fitToOutput(
            output->getProjector(), size.x, size.y, framed ? &lo : nullptr, framed ? &hi : nullptr);
    }
}

void Application::openProjectDialog(bool save, std::function<void(const std::string&)> onChosen) {
    if (!m_projectDialog) m_projectDialog = std::make_unique<AsyncFileDialog>();
    if (m_projectDialog->open(projectFileRequest(save, m_editorState->projectPath))) {
        m_onProjectFileChosen = std::move(onChosen);
    }
}

void Application::processPendingFileAction() {
    using FileAction = EditorState::FileAction;
    EditorState& es = *m_editorState;

    // While the Open/Save dialog is up the editor and projectors keep running, but
    // file actions wait for it; once it closes, its choice is applied here.
    if (m_projectDialog && m_projectDialog->isBusy()) {
        std::vector<std::string> chosen;
        if (!m_projectDialog->takeResult(chosen)) return;
        auto onChosen = std::move(m_onProjectFileChosen);
        m_onProjectFileChosen = nullptr;
        if (onChosen) onChosen(chosen.empty() ? std::string() : chosen.front());
        return;
    }

    const FileAction action = es.pendingFileAction;
    if (action == FileAction::None) return;
    es.pendingFileAction = FileAction::None;

    // Anything that leaves the current project first settles its unsaved changes.
    // Open only picks a file; the Load it queues settles unsaved changes at that
    // point, so edits made while the dialog was up are covered too.
    const bool leavesProject = action == FileAction::New || action == FileAction::Load ||
                               action == FileAction::Exit;
    if (leavesProject) {
        if (es.saveBeforeAction) {
            es.saveBeforeAction = false;
            // Once saved (possibly after the Save dialog) the action is queued again
            // and runs against the now-clean project; cancelled or failed: stay put.
            performSaveProject(false, [&es, action]() { es.pendingFileAction = action; });
            return;
        } else if (!es.discardConfirmed && isProjectDirty()) {
            es.unsavedPromptFor = action;  // renderSessionDialogs() asks, then re-queues
            return;
        }
        es.discardConfirmed = false;
    }

    // The project being left: its autosave/crash files are settled once we move on.
    const std::string leaving = m_project->getFilePath();
    switch (action) {
        case FileAction::New:
            discardRecoveryFiles(es, leaving);
            performNewProject();
            break;
        case FileAction::Open:
            openProjectDialog(false, [this](const std::string& path) {
                if (path.empty()) return;
                EditorState& state = *m_editorState;
                if (state.pendingFileAction != FileAction::None) {
                    // Exit/Save/New queued while the dialog was up wins.
                    postStatus(state, "Open cancelled: another file action is pending");
                    return;
                }
                state.pendingLoadPath = path;
                state.pendingFileAction = FileAction::Load;
            });
            break;
        case FileAction::Load: {
            const std::string path = es.pendingLoadPath;
            es.pendingLoadPath.clear();
            if (path.empty()) break;
            discardRecoveryFiles(es, leaving);
            performLoadProject(path);
            break;
        }
        case FileAction::Save:
            performSaveProject(false);
            break;
        case FileAction::SaveAs:
            performSaveProject(true);
            break;
        case FileAction::Exit:
            discardRecoveryFiles(es, leaving);
            m_exitConfirmed = true;
            glfwSetWindowShouldClose(m_window, GLFW_TRUE);
            break;
        case FileAction::RestoreRecovery:
            performRestoreRecovery();
            break;
        case FileAction::ImportObject:
            if (!m_projectDialog) m_projectDialog = std::make_unique<AsyncFileDialog>();
            if (m_projectDialog->open(objectFileRequest())) {
                m_onProjectFileChosen = [this](const std::string& path) {
                    if (path.empty()) return;
                    importObjectFile(*m_editorState, m_outputManager.get(), m_project.get(), path);
                };
            }
            break;
        case FileAction::DiscardRecovery: {
            const std::string project = es.recoveryProjectPath;
            es.recoveryFilePath.clear();
            es.recoveryProjectPath.clear();
            discardRecoveryFiles(es, project);
            break;
        }
        case FileAction::None:
            break;
    }
}

void Application::performNewProject() {
    EditorState& es = *m_editorState;

    // Undo commands hold raw quad pointers into the scenes about to be destroyed.
    es.commandHistory.clear();
    es.selectedObjectId = UUID();
    es.endCalibration();
    es.scenes.clear();
    es.quadTextures.clear();
    es.quadTexturePaths.clear();
    es.currentTexturePath[0] = '\0';
    es.activeVideoSourceId = UUID();
    es.midiMappings.clear();  // show content; the Control panel's device choices stay
    dropQueuedSceneTakes();
    if (m_sourceManager) releaseProjectSources(es, *m_sourceManager);
    cancelMissingMediaSearch();
    clearMissingMedia(es);

    // One default scene with one quad, still aimed at the first output. Outputs are
    // hardware setup, not show content, so New keeps them. Their projector calibration is show
    // content (it belongs to the objects it was made for), so it starts over.
    if (m_outputManager) {
        for (const auto& output : m_outputManager->getOutputs()) {
            if (!output) continue;
            output->getProjector() = ProjectorCamera{};
            output->getCalibrationPoints().clear();
        }
    }
    es.endCalibration();
    Scene& scene = es.activeScene();
    es.setActiveScene(scene.getId());
    Quad* quad = scene.getQuadManager().createQuad("Quad 1");
    if (quad) scene.getQuadManager().selectQuad(quad->getId());
    if (m_outputManager && !m_outputManager->getOutputs().empty()) {
        scene.setAssignedOutputId(m_outputManager->getOutputs().front()->getId());
    }

    *m_project = Project();
    applyProjectAudio();  // full volume, unmuted
    es.projectPath[0] = '\0';
    markProjectClean();
    postStatus(es, "New project created");
}

bool Application::performLoadProject(const std::string& path) {
    EditorState& es = *m_editorState;
    auto result = ProjectSerializer::load(pathFromUtf8(path));
    if (!result) {
        Logger::error("Load failed: {}", result.error().message);
        postStatus(es, "Load failed: " + result.error().message);
        return false;
    }

    es.commandHistory.clear();  // before the scenes its commands point into go away
    *m_project = std::move(result.value());
    dropQueuedSceneTakes();
    cancelMissingMediaSearch();
    syncProjectToEditor(es, m_outputManager.get(), *m_project, m_sourceManager.get());
    applyProjectAudio();
    snprintf(es.projectPath, sizeof(es.projectPath), "%s", path.c_str());
    markProjectClean();
    const size_t missing = collectMissingMedia(es).size();
    es.showMissingMedia = missing > 0;
    postStatus(es, "Project loaded: " + path + missingMediaNote(missing));
    offerRecovery(path);
    return true;
}

void Application::performSaveProject(bool chooseNewPath, std::function<void()> onSaved) {
    EditorState& es = *m_editorState;
    // The open project's own file. es.projectPath is the Project tab's editable
    // "path to load" field and may name a different show; saving there (e.g. the
    // save-before-load prompt) would overwrite that show with this one.
    const std::string path = m_project->getFilePath();
    if (chooseNewPath || path.empty()) {
        openProjectDialog(true, [this, onSaved = std::move(onSaved)](const std::string& chosen) {
            if (chosen.empty()) {
                postStatus(*m_editorState, "Save cancelled");
                return;
            }
            if (writeProjectFile(chosen) && onSaved) onSaved();
        });
        return;
    }
    if (writeProjectFile(path) && onSaved) onSaved();
}

bool Application::writeProjectFile(const std::string& path) {
    EditorState& es = *m_editorState;
    const std::string previousFile = m_project->getFilePath();
    buildEditorProject(*m_project);
    auto result = ProjectSerializer::save(*m_project, pathFromUtf8(path));
    if (!result) {
        Logger::error("Save failed: {}", result.error().message);
        postStatus(es, "Save failed: " + result.error().message);
        return false;
    }

    m_project->setFilePath(path);
    m_project->setModified(false);
    snprintf(es.projectPath, sizeof(es.projectPath), "%s", path.c_str());
    markProjectClean();
    // The real file now holds everything the autosave did.
    discardRecoveryFiles(es, previousFile);
    discardRecoveryFiles(es, path);
    postStatus(es, "Project saved: " + path);
    return true;
}

void Application::performRestoreRecovery() {
    EditorState& es = *m_editorState;
    const std::string file = es.recoveryFilePath;
    const std::string projectPath = es.recoveryProjectPath;
    es.recoveryFilePath.clear();
    es.recoveryProjectPath.clear();

    auto result = ProjectSerializer::load(pathFromUtf8(file));
    if (!result) {
        Logger::error("Recovery failed: {}", result.error().message);
        postStatus(es, "Could not restore " + file + ": " + result.error().message);
        return;
    }
    es.commandHistory.clear();
    *m_project = std::move(result.value());
    m_project->setFilePath(projectPath);  // it belongs to the project, not the autosave
    dropQueuedSceneTakes();
    cancelMissingMediaSearch();
    syncProjectToEditor(es, m_outputManager.get(), *m_project, m_sourceManager.get());
    applyProjectAudio();
    snprintf(es.projectPath, sizeof(es.projectPath), "%s", projectPath.c_str());
    // The restored work is not in the project file yet: keep it "unsaved", and say so.
    m_savedSnapshot.clear();
    m_project->setModified(true);
    const size_t missing = collectMissingMedia(es).size();
    es.showMissingMedia = missing > 0;
    postStatus(es, "Restored unsaved work from " + file + missingMediaNote(missing));
}

void Application::buildEditorProject(Project& out) {
    syncEditorToProject(*m_editorState, m_outputManager.get(), m_sourceManager.get(), out);
    if (m_audioEngine) {
        out.getAudio().masterVolume = m_audioEngine->masterVolume();
        out.getAudio().masterMuted = m_audioEngine->masterMuted();
    }
}

// Once per frame after the sources advance (ADR-050 §2). Heard: sources of the active
// scene and of scenes on enabled outputs; the rest are gated silent, still in step.
void Application::syncAudio() {
    if (!m_audioEngine || !m_sourceManager || !m_editorState || !m_outputManager) return;
    const EditorState& es = *m_editorState;
    syncSourceAudio(*m_audioEngine, *m_sourceManager,
                    audibleSourceIds(es.scenes, es.activeSceneId, enabledOutputIds(m_outputManager->getOutputs())));
}

void Application::applyProjectAudio() {
    if (!m_audioEngine || !m_project) return;
    m_audioEngine->setMasterVolume(m_project->getAudio().masterVolume);
    m_audioEngine->setMasterMuted(m_project->getAudio().masterMuted);
}

// The output device is a machine preference (ADR-050 §5): written when the user picks
// another one in the menu bar's audio menu or the Preferences window.
void Application::saveAudioDevicePreference() {
    std::string& saved = m_preferences->getPreferences().performance.audioDevice;
    if (!m_audioEngine || m_audioEngine->preferredDevice() == saved) return;
    saved = m_audioEngine->preferredDevice();
    m_preferences->savePreferences();
}

// What counts as an unsaved change: the show's content, not where the viewport is
// panned/zoomed or whether a video happens to be playing right now.
static std::string contentFingerprint(Project p) {
    for (auto& scene : p.getScenes()) scene.view = Project::SceneViewData{};
    // An uncalibrated projector's pose is re-derived every frame (framed on the scene),
    // so it is not an edit of its own.
    for (auto& output : p.getOutputs()) {
        if (!output.projector.calibrated) output.projector.pose = ProjectorPose{};
    }
    for (auto& source : p.getSources()) {
        if (source.transport) source.transport->playing = false;
    }
    return ProjectSerializer().serialize(p);
}

std::string Application::editorSnapshot() {
    Project p;
    p.setName(m_project->getName());
    buildEditorProject(p);
    return contentFingerprint(std::move(p));
}

// Dirty = the editor no longer serializes to what was last saved/loaded. Comparing
// snapshots (only on New/Open/Exit and the autosave tick) catches every edit path,
// including ones that never call Project::setModified().
bool Application::isProjectDirty() {
    return editorSnapshot() != m_savedSnapshot;
}

void Application::markProjectClean() {
    m_savedSnapshot = editorSnapshot();
    m_autosavedSnapshot.clear();
    m_lastAutosaveTime = glfwGetTime();
}

void Application::autosaveIfDue() {
    // Read each tick, so a change in Preferences applies at once (minutes on disk).
    const GeneralPreferences& general = m_preferences->getPreferences().general;
    if (!m_autoSaveEnabled || !general.autoSaveEnabled) return;
    const double interval = 60.0 * std::max(MIN_AUTOSAVE_MINUTES, general.autoSaveIntervalMinutes);
    const double now = glfwGetTime();
    if (now - m_lastAutosaveTime < interval) return;
    m_lastAutosaveTime = now;

    // The previous write is still flushing: try again next tick.
    if (m_autosaveWrite.valid() &&
        m_autosaveWrite.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    finishAutosaveWrite();

    Project p;
    p.setName(m_project->getName());
    buildEditorProject(p);
    const std::string snapshot = contentFingerprint(p);
    if (snapshot == m_savedSnapshot || snapshot == m_autosavedSnapshot) return;

    const fs::path file = recoveryFileFor(m_project->getFilePath(), "autosave");
    auto text = ProjectSerializer::serializeFor(p, file);
    if (!text) {
        Logger::warning("Autosave failed: {}", text.error().message);
        return;
    }
    // The atomic write flushes through to disk (FlushFileBuffers + WRITE_THROUGH),
    // which can take tens of ms; keep it off the render thread so outputs never
    // hitch during a show.
    m_autosavedSnapshot = snapshot;
    m_autosaveWrite = std::async(std::launch::async,
        [file, body = std::move(text.value())]() -> Result<void, Error> {
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            auto result = FileSystem::writeFileAtomic(file, body);
            if (result) Logger::info("Autosaved to {}", utf8FromPath(file));
            return result;
        });
}

void Application::finishAutosaveWrite() {
    if (!m_autosaveWrite.valid()) return;
    auto result = m_autosaveWrite.get();
    if (!result) {
        Logger::warning("Autosave failed: {}", result.error().message);
        m_autosavedSnapshot.clear();  // retry on the next tick
    }
}

void Application::offerRecovery(const std::string& projectPath) {
    EditorState& es = *m_editorState;
    std::error_code ec;
    fs::file_time_type projectTime = fs::file_time_type::min();
    if (!projectPath.empty()) {
        projectTime = fs::last_write_time(pathFromUtf8(projectPath), ec);
        if (ec) return;
    }
    // Newest of the autosave and the crash file, if newer than the project itself.
    fs::path best;
    fs::file_time_type bestTime = projectTime;
    for (const char* kind : {"autosave", "crash"}) {
        const fs::path file = recoveryFileFor(projectPath, kind);
        const auto t = fs::last_write_time(file, ec);
        if (!ec && t > bestTime) {
            best = file;
            bestTime = t;
        }
    }
    if (best.empty()) return;
    es.recoveryFilePath = utf8FromPath(best);
    es.recoveryProjectPath = projectPath;
}

void Application::renderSessionDialogs() {
    using FileAction = EditorState::FileAction;
    EditorState& es = *m_editorState;

    constexpr const char* kUnsavedPopup = "Unsaved Changes##session";
    if (es.unsavedPromptFor != FileAction::None && !ImGui::IsPopupOpen(kUnsavedPopup)) {
        ImGui::OpenPopup(kUnsavedPopup);
    }
    if (ImGui::BeginPopupModal(kUnsavedPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This project has unsaved changes.");
        ImGui::TextUnformatted("Save them before continuing?");
        ImGui::Spacing();
        const FileAction deferred = es.unsavedPromptFor;
        if (ImGui::Button("Save")) {
            es.saveBeforeAction = true;
            es.pendingFileAction = deferred;
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save")) {
            es.discardConfirmed = true;
            es.pendingFileAction = deferred;
        }
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (es.pendingFileAction != FileAction::None || cancel) {
            es.unsavedPromptFor = FileAction::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    constexpr const char* kRecoveryPopup = "Recover Unsaved Work##session";
    if (!es.recoveryFilePath.empty() && !ImGui::IsPopupOpen(kRecoveryPopup)) {
        ImGui::OpenPopup(kRecoveryPopup);
    }
    if (ImGui::BeginPopupModal(kRecoveryPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(es.recoveryProjectPath.empty()
            ? "QUADmapper found unsaved work from an untitled project."
            : "QUADmapper found unsaved work newer than this project file.");
        ImGui::TextDisabled("%s", es.recoveryFilePath.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Restore")) {
            es.pendingFileAction = FileAction::RestoreRecovery;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            es.pendingFileAction = FileAction::DiscardRecovery;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// A plain window, not a modal: the unsaved-changes and recovery modals can open over
// it, and closed it draws nothing, so it never holds the canvas keys.
void Application::renderMissingMediaDialog() {
    EditorState& es = *m_editorState;
    if (!es.showMissingMedia) return;
    const std::vector<MissingMediaItem> items = collectMissingMedia(es);
    const bool searching = m_mediaSearch.valid();
    if (items.empty() && !searching) {
        es.showMissingMedia = false;  // all found, or the project that had them is gone
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 380.0f), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::Begin("Missing Media##relocate", &open,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextWrapped("%zu file%s used by this project could not be opened. What uses them "
                           "shows a magenta checker here and black on the projectors until the "
                           "file is found. Saving keeps the original paths, so nothing is lost "
                           "if you skip this.",
                           items.size(), items.size() == 1 ? "" : "s");
        const bool busy = searching || (m_projectDialog && m_projectDialog->isBusy());
        if (searching) ImGui::TextDisabled("Searching %s ...", m_mediaSearchFolder.c_str());
        ImGui::Spacing();

        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##missing", 3, tableFlags, ImVec2(0.0f, -footer))) {
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Used by", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < items.size(); ++i) {
                const MissingMediaItem& item = items[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(utf8FromPath(pathFromUtf8(item.path).filename()).c_str());
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", item.path.c_str());
                auto error = es.missingMediaErrors.find(item.path);
                if (error != es.missingMediaErrors.end()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error->second.c_str());
                }
                ImGui::PopTextWrapPos();

                ImGui::TableNextColumn();
                std::string usedBy;
                if (item.quadUses > 0) {
                    usedBy = std::to_string(item.quadUses) + (item.quadUses == 1 ? " quad" : " quads");
                }
                if (item.objectUses > 0) {
                    if (!usedBy.empty()) usedBy += ", ";
                    usedBy += std::to_string(item.objectUses) +
                              (item.objectUses == 1 ? " 3D object" : " 3D objects");
                }
                if (item.kind == MissingMediaItem::Kind::Mesh) usedBy += " (mesh)";
                ImGui::TextUnformatted(usedBy.c_str());

                ImGui::TableNextColumn();
                ImGui::BeginDisabled(busy);
                if (ImGui::Button("Locate...")) requestLocateMissingMedia(item);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Search Folder...")) requestMissingMediaSearch();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Pick a folder: every missing file with the same name in it,\n"
                              "or in any folder below it, is relocated.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip")) open = false;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Keep working without these files. File > Relocate Missing Media...\n"
                              "reopens this window.");
        }
    }
    ImGui::End();
    if (!open) es.showMissingMedia = false;
}

void Application::requestLocateMissingMedia(const MissingMediaItem& item) {
    EditorState& es = *m_editorState;
    if (!m_projectDialog) m_projectDialog = std::make_unique<AsyncFileDialog>();
    if (!m_projectDialog->open(locateFileRequest(item))) {
        postStatus(es, "Another file dialog is already open");
        return;
    }
    m_onProjectFileChosen = [this, item](const std::string& path) {
        if (path.empty()) return;
        EditorState& state = *m_editorState;
        const int resolved =
            locateMissingMedia(state, m_sourceManager.get(), m_project.get(), item, pathFromUtf8(path));
        if (resolved > 0) {
            postStatus(state, "Relocated " + std::to_string(resolved) + " missing file" +
                                  (resolved == 1 ? "" : "s"));
        } else {
            postStatus(state, "Could not use " + utf8FromPath(pathFromUtf8(path).filename()));
        }
    };
}

void Application::requestMissingMediaSearch() {
    EditorState& es = *m_editorState;
    if (!m_projectDialog) m_projectDialog = std::make_unique<AsyncFileDialog>();
    if (!m_projectDialog->open(searchFolderRequest())) {
        postStatus(es, "Another file dialog is already open");
        return;
    }
    m_onProjectFileChosen = [this](const std::string& folder) {
        if (folder.empty() || m_mediaSearch.valid()) return;
        m_mediaSearchCancel = false;
        m_mediaSearchFolder = folder;
        m_mediaSearch = std::async(std::launch::async, [this, root = pathFromUtf8(folder)]() {
            return relocate::indexFolder(root, &m_mediaSearchCancel);
        });
    };
}

void Application::pollMissingMediaSearch() {
    if (!m_mediaSearch.valid() ||
        m_mediaSearch.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    const relocate::FolderIndex index = m_mediaSearch.get();
    EditorState& es = *m_editorState;
    const size_t missing = collectMissingMedia(es).size();
    const int resolved = relocateMissingFromIndex(es, m_sourceManager.get(), m_project.get(), index);
    std::string status = "Found " + std::to_string(resolved) + " of " + std::to_string(missing) +
                         " missing file" + (missing == 1 ? "" : "s") + " in " + m_mediaSearchFolder;
    if (index.truncated) status += " (search stopped early: folder too large)";
    postStatus(es, status);
}

void Application::cancelMissingMediaSearch() {
    if (!m_mediaSearch.valid()) return;
    m_mediaSearchCancel = true;
    m_mediaSearch.wait();
    m_mediaSearch = std::future<relocate::FolderIndex>();
}

void Application::emergencySave() noexcept {
    try {
        if (!m_editorState || !m_project) return;
        Project p;
        p.setName(m_project->getName());
        buildEditorProject(p);
        const fs::path file = recoveryFileFor(m_project->getFilePath(), "crash");
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        auto result = ProjectSerializer::save(p, file);
        if (result) {
            Logger::warning("Emergency save written to {}", utf8FromPath(file));
        } else {
            Logger::error("Emergency save failed: {}", result.error().message);
        }
    } catch (...) {
        Logger::error("Emergency save failed with an exception");
    }
}

void Application::aiDetectSurface() {
    EditorState& es = *m_editorState;
    auto setStatus = [&](const std::string& msg, float secs = 5.0f) {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", msg.c_str());
        es.statusTimer = secs;
        Logger::info(msg);
    };

    // Preconditions: a running sidecar and a target quad to warp.
    if (!m_sidecarClient ||
        (m_sidecarClient->state() != ai::SidecarState::Attached &&
         m_sidecarClient->state() != ai::SidecarState::Launched)) {
        setStatus("AI: enable the sidecar first (AI > Enable AI Assist)");
        return;
    }
    Quad* quad = es.quadManager().getPrimarySelectedQuad();
    if (!quad) {
        setStatus("AI: select a quad to warp first");
        return;
    }

    // Resolve the input image. QM_AI_TEST_IMAGE feeds a fixed photo (verification,
    // and the "detect from a phone photo on disk" path); otherwise capture a live
    // frame off the first camera (the QM-055 rail). NOTE: this blocks the UI for
    // ~1-3s (camera open + capture + inference) — acceptable pre-show; async is
    // tracked as TD-10.
    std::filesystem::path imgPath;
    if (const char* testImg = std::getenv("QM_AI_TEST_IMAGE")) {
        imgPath = testImg;
        if (!std::filesystem::exists(imgPath)) {
            setStatus("AI: QM_AI_TEST_IMAGE not found");
            return;
        }
    } else {
        imgPath = std::filesystem::temp_directory_path() / "quadmapper_ai_capture.png";
        std::string err;
        if (!captureCameraFrameToPng(utf8FromPath(imgPath), err)) {
            setStatus("AI: " + err);
            return;
        }
    }

    auto surface = m_sidecarClient->requestSurface(utf8FromPath(imgPath));
    if (!surface) {
        setStatus("AI: detection failed (sidecar error)");
        return;
    }

    // Low confidence: do NOT auto-warp onto a possibly-wrong surface — ask the
    // user to disambiguate instead (the spike's load-bearing human-confirm step).
    constexpr float kConfidenceThreshold = 0.35f;
    if (surface->confidence < kConfidenceThreshold) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "AI: low confidence (%.2f) - tap the surface to retry",
                 surface->confidence);
        setStatus(msg);
        return;
    }

    // Apply the proposed corners (TL,TR,BR,BL, normalized [0,1] — same order and
    // space as QuadGeometry). The result is an ordinary warp: drawn on canvas,
    // F11-projectable, and refinable with Tab-cycle + arrow-nudge.
    std::array<Vec2, 4> corners{
        Vec2{surface->corners[0][0], surface->corners[0][1]},
        Vec2{surface->corners[1][0], surface->corners[1][1]},
        Vec2{surface->corners[2][0], surface->corners[2][1]},
        Vec2{surface->corners[3][0], surface->corners[3][1]},
    };
    quad->getGeometry().setCorners(corners);
    m_project->setModified(true);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "AI: surface applied (confidence %.2f) - nudge with Tab + arrows",
             surface->confidence);
    setStatus(msg);
}

bool Application::captureCameraFrameToPng(const std::string& pngPath,
                                          std::string& error) {
    const auto cameras = enumerateCameraDevices();
    if (cameras.empty()) {
        error = "no camera found (connect one or set QM_AI_TEST_IMAGE)";
        return false;
    }
    CameraSource camera(cameras.front().name);
    if (!camera.initialize()) {
        error = "failed to open camera";
        return false;
    }
    // Pump updates until the first frame lands (or give up after ~1s).
    for (int i = 0; i < 60 && !camera.hasNewFrame(); ++i) {
        camera.update(0.016);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const bool captured = camera.captureCurrentFrameToPng(pathFromUtf8(pngPath));
    camera.shutdown();
    if (!captured) {
        error = "camera capture failed (no frame)";
        return false;
    }
    return true;
}

void Application::oneFlashEmitPattern() {
    EditorState& es = *m_editorState;
    auto setStatus = [&](const std::string& msg) {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", msg.c_str());
        es.statusTimer = 5.0f;
        Logger::info(msg);
    };

    // Lazily load the calibration pattern + build the fullscreen identity quad.
    if (!m_calibTexture) {
        std::filesystem::path patternPath;
        const std::filesystem::path rel("resources/patterns/charuco.png");
        for (const std::filesystem::path& base :
             {std::filesystem::current_path(),
              std::filesystem::current_path() / "..",
              std::filesystem::current_path() / ".." / ".."}) {
            std::error_code ec;
            if (std::filesystem::exists(base / rel, ec)) {
                patternPath = base / rel;
                break;
            }
        }
        auto tex = std::make_unique<Texture>();
        if (patternPath.empty() || !tex->loadFromFile(patternPath)) {
            setStatus("One-Flash: pattern asset not found (resources/patterns/charuco.png)");
            return;
        }
        m_calibTexture = std::move(tex);
        m_calibQuad = std::make_unique<Quad>();
        m_calibQuad->getGeometry().setCorners(
            {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)});
    }

    m_emitPattern = true;
    if (!m_outputManager->isInPerformMode()) {
        setStatus("One-Flash: pattern armed - press F11 to flash it on the projector");
    } else {
        setStatus("One-Flash: pattern on projector - capture a photo, then Clear Pattern");
    }
}

void Application::oneFlashCapturePhoto() {
    EditorState& es = *m_editorState;
    auto setStatus = [&](const std::string& msg) {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", msg.c_str());
        es.statusTimer = 6.0f;
        Logger::info(msg);
    };

    if (m_outputManager && m_outputManager->isBlackout()) {  // the photo would be black
        setStatus("One-Flash: Blackout is on - turn it off to capture");
        return;
    }
    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "quadmapper_oneflash_capture.png";
    std::string err;
    if (!captureCameraFrameToPng(utf8FromPath(out), err)) {
        setStatus("One-Flash: " + err);
        return;
    }
    setStatus("One-Flash: captured " + utf8FromPath(out));
}

void Application::oneFlashClearPattern() {
    EditorState& es = *m_editorState;
    m_emitPattern = false;
    snprintf(es.statusMessage, sizeof(es.statusMessage), "One-Flash: pattern cleared");
    es.statusTimer = 4.0f;
    Logger::info("One-Flash: pattern cleared");
}

void Application::oneFlashSolveAndApply() {
    runOneFlash(nullptr, "warp applied");
}

void Application::oneFlashAnamorphic() {
    // A centered rectangle in the audience (photo) view: content mapped here
    // looks like a frontal rectangle from the captured viewpoint.
    static const std::array<std::array<float, 2>, 4> kAudienceTarget{{
        {0.15f, 0.15f}, {0.85f, 0.15f}, {0.85f, 0.85f}, {0.15f, 0.85f}}};
    runOneFlash(&kAudienceTarget, "anamorphic warp applied");
}

void Application::runOneFlash(const std::array<std::array<float, 2>, 4>* audienceTarget,
                              const char* label) {
    EditorState& es = *m_editorState;
    auto setStatus = [&](const std::string& msg) {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", msg.c_str());
        es.statusTimer = 6.0f;
        Logger::info(msg);
    };

    // Under blackout the projectors show no pattern, so the camera would photograph
    // black and the solve would report a detection failure.
    if (m_outputManager && m_outputManager->isBlackout()) {
        setStatus("One-Flash: Blackout is on - turn it off to capture");
        return;
    }
    if (!m_sidecarClient ||
        (m_sidecarClient->state() != ai::SidecarState::Attached &&
         m_sidecarClient->state() != ai::SidecarState::Launched)) {
        setStatus("One-Flash: enable the sidecar first (AI > Enable AI Assist)");
        return;
    }
    Quad* quad = es.quadManager().getPrimarySelectedQuad();
    if (!quad) {
        setStatus("One-Flash: select a quad to warp first");
        return;
    }

    // The pattern photo: QM_AI_TEST_IMAGE override (a synthetic capture for
    // verification) else a live camera shot of the projected pattern.
    std::filesystem::path patternPath;
    if (const char* testImg = std::getenv("QM_AI_TEST_IMAGE")) {
        patternPath = testImg;
        if (!std::filesystem::exists(patternPath)) {
            setStatus("One-Flash: QM_AI_TEST_IMAGE not found");
            return;
        }
    } else {
        patternPath = std::filesystem::temp_directory_path() /
                      "quadmapper_oneflash_capture.png";
        std::string err;
        if (!captureCameraFrameToPng(utf8FromPath(patternPath), err)) {
            setStatus("One-Flash: " + err);
            return;
        }
    }

    auto result = m_sidecarClient->requestOneFlash(utf8FromPath(patternPath), audienceTarget);
    if (!result || result->stub) {
        setStatus("One-Flash: pattern not detected (need the ChArUco visible)");
        return;
    }
    constexpr float kConfidenceThreshold = 0.35f;
    if (result->confidence < kConfidenceThreshold) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "One-Flash: low confidence (%.2f) - reframe the pattern",
                 result->confidence);
        setStatus(msg);
        return;
    }

    std::array<Vec2, 4> corners{
        Vec2{result->corners[0][0], result->corners[0][1]},
        Vec2{result->corners[1][0], result->corners[1][1]},
        Vec2{result->corners[2][0], result->corners[2][1]},
        Vec2{result->corners[3][0], result->corners[3][1]},
    };
    quad->getGeometry().setCorners(corners);
    m_project->setModified(true);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "One-Flash: %s (confidence %.2f) - send to projector (F11)",
             label, result->confidence);
    setStatus(msg);
}

void Application::renderProjectorFrames() {
    if (!m_outputManager || !m_editorState || !m_renderer) {
        return;
    }

    EditorState& es = *m_editorState;

    // One-Flash: when a calibration pattern is armed, flash it fullscreen on the
    // outputs instead of the scene (QM-059).
    if (m_emitPattern && m_calibQuad) {
        m_outputManager->renderPatternFrames(*m_calibQuad, m_calibTexture.get(), *m_renderer);
        return;
    }

    // Resolve every quad's texture (image via SourceManager/mediaId, video via the
    // legacy wrapper) through the shared builder, with the projector's missing-media
    // look (black).
    auto textures = buildQuadTextureMap(es, m_sourceManager.get(), SurfaceView::Output);
    m_outputManager->renderOutputFrames(es.scenes, textures, *m_renderer);
}

void Application::shutdown() {
    if (!m_window) {
        return; // Already shut down
    }

    Logger::info("Shutting down QUADmapper...");
    finishAutosaveWrite();  // never cut an autosave off mid-write
    cancelMissingMediaSearch();

    // Cleanup components in reverse order of creation
    m_externalControl.reset(); // joins the OSC thread, closes MIDI devices
    m_audioEngine.reset();     // stops every video's audio, then closes the device
    m_sidecarClient.reset();   // stop()s / kills any launched sidecar (no orphan)
    m_outputManager.reset();

    // Everything below owns GL objects in the editor's context, so it goes while that
    // context is still alive and current (glfwTerminate below destroys it).
    glfwMakeContextCurrent(m_window);
    // Sources after the audio engine: each video's audio is freed once the mixer can no
    // longer reach it; then the decode and copier threads stop and the textures, YUV
    // planes and pixel buffers are deleted.
    m_sourceManager.reset();
    m_thumbnailCache.reset();
    // Quad textures, missing-media looks, the calibration view, media-bin thumbnails.
    m_editorState.reset();
    m_calibTexture.reset();
    m_renderer.reset();
    m_folderWatcher.reset();  // Stop watching before destroying media library
    m_mediaLibrary.reset();
    m_project.reset();

    // Anything counted still alive now would be deleted after its context is gone.
    const GlObjectStats::Snapshot alive = GlObjectStats::Snapshot::capture();
    if (alive.textures != 0 || alive.framebuffers != 0 || alive.vertexArrays != 0 ||
        alive.vertexBuffers != 0 || alive.shaderPrograms != 0) {
        Logger::warning("GL objects still alive at context teardown:\n{}",
                        alive.diff(GlObjectStats::Snapshot{}));
    }

    // Shutdown ImGui
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    // Destroy window and terminate GLFW
    glfwDestroyWindow(m_window);
    glfwTerminate();

    m_window = nullptr;
    m_running = false;
    Logger::info("Shutdown complete");
}

} // namespace quadmapper
