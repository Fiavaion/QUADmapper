#include "ProjectSession.hpp"
#include "../editor/EditorState.hpp"
#include "../editor/MeshLoads.hpp"
#include "../editor/MissingMedia.hpp"
#include "../editor/ProjectMapping.hpp"
#include "../editor/ProjectReplace.hpp"
#include "../editor/StatusLine.hpp"
#include "../core/FileSystem.hpp"
#include "../core/Logger.hpp"
#include "../project/Project.hpp"
#include "../project/ProjectSerializer.hpp"
#include "../scene/ProjectorCamera.hpp"
#include "../scene/Scene.hpp"
#include "../source/SourceManager.hpp"
#include "../output/OutputManager.hpp"
#include "../output/OutputWindow.hpp"
#include "../audio/AudioEngine.hpp"
#include "../ui/AsyncFileDialog.hpp"
#include "../editor/Preferences.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
// NOMINMAX prevents windows.h from defining min/max macros that clash with std::max.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>   // SHGetKnownFolderPath: recovery folder (ADR-046)

namespace quadmapper {

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

// What counts as an unsaved change: the show's content, not where the viewport is
// panned/zoomed or whether a video happens to be playing right now.
std::string contentFingerprint(Project p) {
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

}  // namespace

ProjectSession::ProjectSession(Project& project, EditorState& editor,
                               const AppPreferences& preferences, Services services)
    : m_project(project), m_editor(editor), m_preferences(preferences),
      m_services(std::move(services)) {}

ProjectSession::~ProjectSession() = default;

bool ProjectSession::ensureCleanBaseline() {
    if (m_haveCleanBaseline) return true;
    if (!m_editor.initialized) return false;
    markProjectClean();
    m_haveCleanBaseline = true;
    offerRecovery("");
    return true;
}

bool ProjectSession::unsavedLabelShown(double now) {
    return m_unsavedLabel.shown(now, [this] { return isProjectDirty(); });
}

bool ProjectSession::isFileDialogBusy() const {
    return m_projectDialog && m_projectDialog->isBusy();
}

void ProjectSession::dropQueuedSceneTakes() {
    if (m_services.dropQueuedSceneTakes) m_services.dropQueuedSceneTakes();
}

void ProjectSession::projectReplaced() {
    if (m_services.projectReplaced) m_services.projectReplaced();
}

void ProjectSession::openProjectDialog(bool save, std::function<void(const std::string&)> onChosen) {
    if (!m_projectDialog) m_projectDialog = std::make_unique<AsyncFileDialog>();
    if (m_projectDialog->open(projectFileRequest(save, m_editor.projectPath))) {
        m_onProjectFileChosen = std::move(onChosen);
    }
}

void ProjectSession::processPendingFileAction() {
    using FileAction = EditorState::FileAction;
    EditorState& es = m_editor;

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
    const std::string leaving = m_project.getFilePath();
    switch (action) {
        case FileAction::New:
            discardRecoveryFiles(es, leaving);
            performNewProject();
            break;
        case FileAction::Open:
            openProjectDialog(false, [this](const std::string& path) {
                if (path.empty()) return;
                EditorState& state = m_editor;
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
            if (m_services.confirmExit) m_services.confirmExit();
            break;
        case FileAction::RestoreRecovery:
            performRestoreRecovery();
            break;
        case FileAction::ImportObject:
            if (!m_projectDialog) m_projectDialog = std::make_unique<AsyncFileDialog>();
            if (m_projectDialog->open(objectFileRequest())) {
                m_onProjectFileChosen = [this](const std::string& path) {
                    if (path.empty()) return;
                    importObjectFile(m_editor, path);
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

void ProjectSession::performNewProject() {
    EditorState& es = m_editor;

    dropQueuedSceneTakes();
    cancelMissingMediaSearch();
    clearProjectContent(es, m_services.sources, m_services.outputs, m_services.library);
    es.midiMappings.clear();  // show content; the Control panel's device choices stay

    // One default scene with one quad, still aimed at the first output. Outputs are
    // hardware setup, not show content, so New keeps them. Their projector calibration,
    // routing and slices are show content (saved in the .qmap), so they start over.
    if (m_services.outputs) {
        for (const auto& output : m_services.outputs->getOutputs()) {
            if (!output) continue;
            output->getProjector() = ProjectorCamera{};
            output->getCalibrationPoints().clear();
            output->setSceneId(UUID::null());
            output->getSlices().clear();
        }
    }
    es.endCalibration();
    Scene& scene = es.activeScene();
    es.setActiveScene(scene.getId());
    Quad* quad = scene.getQuadManager().createQuad("Quad 1");
    if (quad) scene.getQuadManager().selectQuad(quad->getId());
    if (m_services.outputs && !m_services.outputs->getOutputs().empty()) {
        OutputWindow& first = *m_services.outputs->getOutputs().front();
        scene.setCanvasSize(first.getEffectiveResolution());
        OutputManager::showScene(first, &scene, es.scenes);
    }

    m_project = Project();
    applyProjectAudio();  // full volume, unmuted
    es.projectPath[0] = '\0';
    markProjectClean();
    postStatus(es, "New project created");
    projectReplaced();
}

bool ProjectSession::performLoadProject(const std::string& path) {
    EditorState& es = m_editor;
    auto result = ProjectSerializer::load(pathFromUtf8(path));
    if (!result) {
        Logger::error("Load failed: {}", result.error().message);
        postStatus(es, "Load failed: " + result.error().message);
        return false;
    }

    m_project = std::move(result.value());
    dropQueuedSceneTakes();
    cancelMissingMediaSearch();
    syncProjectToEditor(es, m_services.outputs, m_project, m_services.sources, m_services.library);
    applyProjectAudio();
    snprintf(es.projectPath, sizeof(es.projectPath), "%s", path.c_str());
    markProjectClean();
    const size_t missing = collectMissingMedia(es).size();
    if (missing > 0) es.requestPanelOpen(EditorState::MISSING_MEDIA_PANEL_ID);
    postStatus(es, "Project loaded: " + path + missingMediaNote(missing));
    offerRecovery(path);
    projectReplaced();
    return true;
}

void ProjectSession::performSaveProject(bool chooseNewPath, std::function<void()> onSaved) {
    // The open project's own file. es.projectPath is the Project tab's editable
    // "path to load" field and may name a different show; saving there (e.g. the
    // save-before-load prompt) would overwrite that show with this one.
    const std::string path = m_project.getFilePath();
    if (chooseNewPath || path.empty()) {
        openProjectDialog(true, [this, onSaved = std::move(onSaved)](const std::string& chosen) {
            if (chosen.empty()) {
                postStatus(m_editor, "Save cancelled");
                return;
            }
            if (writeProjectFile(chosen) && onSaved) onSaved();
        });
        return;
    }
    if (writeProjectFile(path) && onSaved) onSaved();
}

bool ProjectSession::writeProjectFile(const std::string& path) {
    EditorState& es = m_editor;
    const std::string previousFile = m_project.getFilePath();
    buildEditorProject(m_project);
    auto result = ProjectSerializer::save(m_project, pathFromUtf8(path));
    if (!result) {
        Logger::error("Save failed: {}", result.error().message);
        postStatus(es, "Save failed: " + result.error().message);
        return false;
    }

    m_project.setFilePath(path);
    snprintf(es.projectPath, sizeof(es.projectPath), "%s", path.c_str());
    markProjectClean();
    // The real file now holds everything the autosave did.
    discardRecoveryFiles(es, previousFile);
    discardRecoveryFiles(es, path);
    postStatus(es, "Project saved: " + path);
    return true;
}

void ProjectSession::performRestoreRecovery() {
    EditorState& es = m_editor;
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
    m_project = std::move(result.value());
    m_project.setFilePath(projectPath);  // it belongs to the project, not the autosave
    dropQueuedSceneTakes();
    cancelMissingMediaSearch();
    syncProjectToEditor(es, m_services.outputs, m_project, m_services.sources, m_services.library);
    applyProjectAudio();
    snprintf(es.projectPath, sizeof(es.projectPath), "%s", projectPath.c_str());
    // The restored work is not in the project file yet: keep it "unsaved", and say so.
    m_savedSnapshot.clear();
    m_unsavedLabel.invalidate();
    const size_t missing = collectMissingMedia(es).size();
    if (missing > 0) es.requestPanelOpen(EditorState::MISSING_MEDIA_PANEL_ID);
    postStatus(es, "Restored unsaved work from " + file + missingMediaNote(missing));
    projectReplaced();
}

void ProjectSession::buildEditorProject(Project& out) {
    syncEditorToProject(m_editor, m_services.outputs, m_services.sources, out);
    if (m_services.audio) {
        out.getAudio().masterVolume = m_services.audio->masterVolume();
        out.getAudio().masterMuted = m_services.audio->masterMuted();
    }
}

void ProjectSession::applyProjectAudio() {
    if (!m_services.audio) return;
    m_services.audio->setMasterVolume(m_project.getAudio().masterVolume);
    m_services.audio->setMasterMuted(m_project.getAudio().masterMuted);
}

std::string ProjectSession::editorSnapshot() {
    Project p;
    p.setName(m_project.getName());
    buildEditorProject(p);
    return contentFingerprint(std::move(p));
}

// Dirty = the editor no longer serializes to what was last saved/loaded. Comparing
// snapshots (only on New/Open/Exit and the autosave tick) catches every edit path,
// with no need for callers to flag individual edits.
bool ProjectSession::isProjectDirty() {
    return editorSnapshot() != m_savedSnapshot;
}

void ProjectSession::markProjectClean() {
    m_savedSnapshot = editorSnapshot();
    m_unsavedLabel.invalidate();
    m_autosavedSnapshot.clear();
    m_lastAutosaveTime = glfwGetTime();
}

void ProjectSession::autosaveIfDue() {
    // Read each tick, so a change in Preferences applies at once (minutes on disk).
    const GeneralPreferences& general = m_preferences.general;
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
    p.setName(m_project.getName());
    buildEditorProject(p);
    const std::string snapshot = contentFingerprint(p);
    if (snapshot == m_savedSnapshot || snapshot == m_autosavedSnapshot) return;

    const fs::path file = recoveryFileFor(m_project.getFilePath(), "autosave");
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

void ProjectSession::finishAutosaveWrite() {
    if (!m_autosaveWrite.valid()) return;
    auto result = m_autosaveWrite.get();
    if (!result) {
        Logger::warning("Autosave failed: {}", result.error().message);
        m_autosavedSnapshot.clear();  // retry on the next tick
    }
}

void ProjectSession::offerRecovery(const std::string& projectPath) {
    EditorState& es = m_editor;
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

void ProjectSession::requestLocateMissingMedia(const MissingMediaItem& item) {
    EditorState& es = m_editor;
    if (!m_projectDialog) m_projectDialog = std::make_unique<AsyncFileDialog>();
    if (!m_projectDialog->open(locateFileRequest(item))) {
        postStatus(es, "Another file dialog is already open");
        return;
    }
    m_onProjectFileChosen = [this, item](const std::string& path) {
        if (path.empty()) return;
        EditorState& state = m_editor;
        const int resolved =
            locateMissingMedia(state, m_services.sources, item, pathFromUtf8(path));
        if (resolved > 0) {
            postStatus(state, "Relocated " + std::to_string(resolved) + " missing file" +
                                  (resolved == 1 ? "" : "s"));
        } else {
            postStatus(state, "Could not use " + utf8FromPath(pathFromUtf8(path).filename()));
        }
    };
}

void ProjectSession::requestMissingMediaSearch() {
    EditorState& es = m_editor;
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

void ProjectSession::pollMissingMediaSearch() {
    if (!m_mediaSearch.valid() ||
        m_mediaSearch.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    const relocate::FolderIndex index = m_mediaSearch.get();
    EditorState& es = m_editor;
    const size_t missing = collectMissingMedia(es).size();
    const int resolved = relocateMissingFromIndex(es, m_services.sources, index);
    std::string status = "Found " + std::to_string(resolved) + " of " + std::to_string(missing) +
                         " missing file" + (missing == 1 ? "" : "s") + " in " + m_mediaSearchFolder;
    if (index.truncated) status += " (search stopped early: folder too large)";
    postStatus(es, status);
}

void ProjectSession::cancelMissingMediaSearch() {
    if (!m_mediaSearch.valid()) return;
    m_mediaSearchCancel = true;
    m_mediaSearch.wait();
    m_mediaSearch = std::future<relocate::FolderIndex>();
}

void ProjectSession::emergencySave() noexcept {
    try {
        Project p;
        p.setName(m_project.getName());
        buildEditorProject(p);
        const fs::path file = recoveryFileFor(m_project.getFilePath(), "crash");
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

}  // namespace quadmapper
