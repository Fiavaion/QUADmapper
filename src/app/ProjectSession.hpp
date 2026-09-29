#pragma once

#include "../core/Result.hpp"
#include "../project/MediaRelocation.hpp"
#include "../editor/UnsavedIndicator.hpp"

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace quadmapper {

class Project;
class SourceManager;
class MediaLibrary;
class OutputManager;
class AudioEngine;
class AsyncFileDialog;
struct EditorState;
struct MissingMediaItem;
struct AppPreferences;

// The show-safe project session (ADR-046 §4, ADR-051 §1): New/Open/Save, autosave,
// crash recovery, the unsaved-changes rule and the emergency save, plus the Missing
// Media locate and folder search. Owned by Application.
//
// Everything that replaces or writes the project runs at a frame boundary
// (processPendingFileAction, called before render() takes any scene references),
// never from inside the ImGui frame: File > Load used to destroy the Scene whose
// QuadManager& render() was still using. Callers only queue an EditorState::FileAction.
// The unsaved-changes and recovery modals and the Missing Media window stay with the
// UI; they read and answer through EditorState and the calls below.
class ProjectSession {
public:
    // What the session reaches beyond the project and the editor. Pointers may be null
    // (the tests run without outputs or audio).
    struct Services {
        SourceManager* sources = nullptr;
        OutputManager* outputs = nullptr;
        AudioEngine* audio = nullptr;
        // The media bin's library: its sources survive New, Load and Restore.
        const MediaLibrary* library = nullptr;
        // New, Load and Restore drop scene takes still queued for the replaced show.
        std::function<void()> dropQueuedSceneTakes;
        // New, Load and Restore replaced the show (after the new one is in place).
        std::function<void()> projectReplaced;
        // Exit passed the unsaved-changes check: close the editor.
        std::function<void()> confirmExit;
    };

    // `preferences` is read on every autosave tick, so a change applies at once.
    ProjectSession(Project& project, EditorState& editor, const AppPreferences& preferences,
                   Services services);
    ~ProjectSession();

    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;

    // ---- Frame boundary, in this order (Application::runFrameBoundaryTasks) --------
    // The clean baseline is the editor after its first frame (the default quad exists
    // by then); recovery is offered from the same point. False until then: skip the
    // rest of the frame boundary.
    bool ensureCleanBaseline();
    void processPendingFileAction();
    // Relocation from a finished folder search.
    void pollMissingMediaSearch();
    void autosaveIfDue();

    // Collect the finished background autosave write (logs a failure). Blocks if it is
    // still running.
    void finishAutosaveWrite();
    // Stop a folder search whose results belong to a project being replaced; waits for
    // the worker to notice.
    void cancelMissingMediaSearch();

    // Crash guard (ADR-046 §4): best-effort write of the live project to
    // <project>.crash.qmap (or the recovery folder when untitled). Never touches the
    // real project file and never throws.
    void emergencySave() noexcept;

    // Dirty = the editor no longer serializes to what was last saved/loaded.
    bool isProjectDirty();
    // The Project tab's "(modified)" label at time `now`, by the same rule.
    bool unsavedLabelShown(double now);
    // An edit changed the show in one step (undo, redo): ask the label again.
    void invalidateUnsavedLabel() { m_unsavedLabel.invalidate(); }

    // ---- Missing media (PRD §4.1.8, MissingMedia.hpp) -------------------------------
    // Locate and Search Folder use the non-blocking picker, and the folder is searched
    // on a worker thread. Relocation itself runs at the frame boundary (the picker
    // callback and pollMissingMediaSearch()).
    void requestLocateMissingMedia(const MissingMediaItem& item);
    void requestMissingMediaSearch();
    bool isFileDialogBusy() const;

    // ---- A picker's answer (empty = cancelled), at the frame boundary --------------
    // The pickers are ownerless, so one can be answered after the perform lock was
    // taken (ADR-051 §4): each of these then refuses, with a status. A folder search
    // that finishes under the lock is held and applied once the show is unlocked.
    void objectFileChosen(const std::string& path);
    void locateFileChosen(const MissingMediaItem& item, const std::string& path);
    void searchFolderChosen(const std::string& folder);
    bool isSearchingMedia() const { return m_mediaSearch.valid(); }
    const std::string& mediaSearchFolder() const { return m_mediaSearchFolder; }

private:
    void performNewProject();
    bool performLoadProject(const std::string& path);
    // Save to the project's path, or via the Save dialog when untitled / chooseNewPath.
    // onSaved runs once the file is written (after the dialog closes, if one was shown).
    void performSaveProject(bool chooseNewPath, std::function<void()> onSaved = {});
    bool writeProjectFile(const std::string& path);
    // Show the .qmap Open/Save dialog without blocking rendering (QM-088); onChosen
    // gets the path (empty = cancelled) from processPendingFileAction() once it closes.
    void openProjectDialog(bool save, std::function<void(const std::string&)> onChosen);
    void performRestoreRecovery();
    // The live editor as a serializable Project DTO, and its canonical JSON text
    // (compared against the last saved text to decide "unsaved changes").
    void buildEditorProject(Project& out);
    std::string editorSnapshot();
    void markProjectClean();
    void applyProjectAudio();  // project master volume/mute -> engine
    // Offer a recovery file for `projectPath` ("" = untitled) newer than the project.
    void offerRecovery(const std::string& projectPath);
    void dropQueuedSceneTakes();
    void projectReplaced();

    // The headless bench switches autosave and the startup baseline off.
    friend class BenchRunner;

    Project& m_project;
    EditorState& m_editor;
    const AppPreferences& m_preferences;
    Services m_services;

    bool m_haveCleanBaseline = false;  // m_savedSnapshot captured after the first frame
    std::string m_savedSnapshot;       // editorSnapshot() as of the last save/load/new
    UnsavedIndicator m_unsavedLabel;   // the Project tab's "(modified)", by the same rule
    std::string m_autosavedSnapshot;   // editorSnapshot() last written to the autosave
    bool m_autoSaveEnabled = true;     // false: never this session (the bench), whatever the preferences
    double m_lastAutosaveTime = 0.0;
    std::future<Result<void, Error>> m_autosaveWrite;  // in-flight autosave disk write
    std::unique_ptr<AsyncFileDialog> m_projectDialog;  // Open/Save .qmap, off the render thread
    std::function<void(const std::string&)> m_onProjectFileChosen;
    // Missing-media folder search in flight (declared in this order so the future,
    // which waits for its worker, is destroyed before the flag the worker reads).
    std::atomic<bool> m_mediaSearchCancel{false};
    std::string m_mediaSearchFolder;
    bool m_mediaSearchHeldNoted = false;  // "held until unlocked" posted for this search
    std::future<relocate::FolderIndex> m_mediaSearch;
};

}  // namespace quadmapper
