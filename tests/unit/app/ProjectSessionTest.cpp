// The show-safe project session (app/ProjectSession.cpp, ADR-046 §4): the order file
// actions settle unsaved changes in, New, Exit and the emergency save, on a real
// EditorState with no outputs, audio or GL. Every test gives the project a file in a
// temp folder first, so no action touches the machine's untitled recovery files.

#include "app/ProjectSession.hpp"
#include "editor/EditorState.hpp"
#include "editor/MissingMedia.hpp"
#include "editor/PerformLock.hpp"
#include "editor/ProjectMapping.hpp"
#include "core/FileSystem.hpp"
#include "core/UUID.hpp"
#include "project/Project.hpp"
#include "project/ProjectSerializer.hpp"
#include "source/SourceManager.hpp"
#include "editor/Preferences.hpp"
#include "editor/EditorContext.hpp"
#include "ui/dialogs/MissingMediaWindow.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace quadmapper;
namespace fs = std::filesystem;

namespace {

using FileAction = EditorState::FileAction;

class ProjectSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("qm_session_" + UUID::generate().toString());
        fs::create_directories(dir_);

        ProjectSession::Services services;
        services.sources = &sources_;
        services.dropQueuedSceneTakes = [this]() { ++takesDropped_; };
        services.confirmExit = [this]() { exitConfirmed_ = true; };
        services.projectReplaced = [this]() { ++replaced_; };
        session_ = std::make_unique<ProjectSession>(project_, es_, prefs_, std::move(services));

        // The editor after its first frame: one quad.
        es_.activeScene().getQuadManager().createQuad("Quad 1");
    }

    void TearDown() override {
        session_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::string file(const char* name) const { return utf8FromPath(dir_ / name); }

    // Open the project as `path` with the editor as it is now as the clean state.
    void openAs(const std::string& path) {
        project_.setFilePath(path);
        snprintf(es_.projectPath, sizeof(es_.projectPath), "%s", path.c_str());
        es_.initialized = true;
        ASSERT_TRUE(session_->ensureCleanBaseline());
        // Only this test's files: never a recovery prompt found on the machine.
        es_.recoveryFilePath.clear();
        es_.recoveryProjectPath.clear();
    }

    size_t quadCount() { return es_.activeScene().getQuadManager().getQuadCount(); }

    fs::path dir_;
    Project project_;
    EditorState es_;
    SourceManager sources_;
    AppPreferences prefs_;
    std::unique_ptr<ProjectSession> session_;
    int takesDropped_ = 0;
    int replaced_ = 0;  // services.projectReplaced calls
    bool exitConfirmed_ = false;
};

}  // namespace

TEST_F(ProjectSessionTest, NoBaselineBeforeTheEditorsFirstFrame) {
    EXPECT_FALSE(session_->ensureCleanBaseline());
    es_.initialized = true;
    EXPECT_TRUE(session_->ensureCleanBaseline());
    EXPECT_FALSE(session_->isProjectDirty());
    es_.activeScene().getQuadManager().createQuad("Quad 2");
    EXPECT_TRUE(session_->isProjectDirty());
}

TEST_F(ProjectSessionTest, NewResetsPathAndShow) {
    openAs(file("old.qmap"));
    es_.activeScene().getQuadManager().createQuad("Quad 2");
    es_.discardConfirmed = true;  // "Don't Save"
    es_.pendingFileAction = FileAction::New;

    session_->processPendingFileAction();

    EXPECT_TRUE(project_.getFilePath().empty());
    EXPECT_STREQ(es_.projectPath, "");
    ASSERT_EQ(quadCount(), 1u);
    EXPECT_EQ(es_.activeScene().getQuadManager().getQuads()[0]->getName(), "Quad 1");
    EXPECT_FALSE(es_.discardConfirmed);
    EXPECT_EQ(takesDropped_, 1);
    EXPECT_STREQ(es_.statusMessage, "New project created");
    EXPECT_FALSE(session_->isProjectDirty());
}

TEST_F(ProjectSessionTest, UnsavedChangesAskBeforeLoading) {
    openAs(file("a.qmap"));
    es_.activeScene().getQuadManager().createQuad("Quad 2");
    es_.pendingLoadPath = file("b.qmap");
    es_.pendingFileAction = FileAction::Load;

    session_->processPendingFileAction();

    EXPECT_EQ(es_.unsavedPromptFor, FileAction::Load);
    EXPECT_EQ(es_.pendingFileAction, FileAction::None);
    EXPECT_EQ(es_.pendingLoadPath, file("b.qmap")) << "nothing loaded until the prompt is answered";
    EXPECT_EQ(quadCount(), 2u);
}

// "Save" in the unsaved-changes prompt: the open show is written to its own file first,
// then the Load is queued again and runs against the now-clean project on the next
// frame boundary. The loaded show must never be what gets saved.
TEST_F(ProjectSessionTest, SaveBeforeLoadSavesTheOpenShowThenLoads) {
    {
        EditorState other;
        other.activeScene().setName("Show B");
        other.activeScene().getQuadManager().createQuad("B1");
        Project b;
        syncEditorToProject(other, nullptr, nullptr, b);
        ASSERT_TRUE(ProjectSerializer::save(b, dir_ / "b.qmap"));
    }
    openAs(file("a.qmap"));
    es_.activeScene().setName("Show A");
    es_.activeScene().getQuadManager().createQuad("Quad 2");
    es_.saveBeforeAction = true;
    es_.pendingLoadPath = file("b.qmap");
    snprintf(es_.projectPath, sizeof(es_.projectPath), "%s", file("b.qmap").c_str());  // typed in the Project tab
    es_.pendingFileAction = FileAction::Load;

    session_->processPendingFileAction();

    ASSERT_TRUE(fs::exists(dir_ / "a.qmap"));
    auto saved = ProjectSerializer::load(dir_ / "a.qmap");
    ASSERT_TRUE(saved);
    ASSERT_EQ(saved.value().getScenes().size(), 1u);
    EXPECT_EQ(saved.value().getScenes()[0].name, "Show A");
    EXPECT_EQ(saved.value().getScenes()[0].quads.size(), 2u);
    EXPECT_EQ(es_.pendingFileAction, FileAction::Load) << "the Load is queued again once saved";
    EXPECT_EQ(es_.activeScene().getName(), "Show A") << "not loaded in the same step";

    session_->processPendingFileAction();

    EXPECT_EQ(es_.activeScene().getName(), "Show B");
    EXPECT_EQ(std::string(es_.projectPath), file("b.qmap"));
    EXPECT_EQ(es_.unsavedPromptFor, FileAction::None);
    EXPECT_EQ(takesDropped_, 1);
    EXPECT_FALSE(session_->isProjectDirty());
}

TEST_F(ProjectSessionTest, ExitClosesOnlyWhenNothingIsUnsaved) {
    openAs(file("show.qmap"));
    es_.activeScene().getQuadManager().createQuad("Quad 2");
    es_.pendingFileAction = FileAction::Exit;
    session_->processPendingFileAction();
    EXPECT_FALSE(exitConfirmed_);
    EXPECT_EQ(es_.unsavedPromptFor, FileAction::Exit);

    es_.unsavedPromptFor = FileAction::None;
    es_.discardConfirmed = true;
    es_.pendingFileAction = FileAction::Exit;
    session_->processPendingFileAction();
    EXPECT_TRUE(exitConfirmed_);
}

// Exit on a dirty show asks first; the autosave must survive until that is answered, or
// a cancelled Exit followed by a crash would leave nothing to recover.
TEST_F(ProjectSessionTest, ADirtyExitKeepsTheAutosaveWhileItAsks) {
    openAs(file("show.qmap"));
    { std::FILE* f = std::fopen(file("show.autosave.qmap").c_str(), "wb"); ASSERT_TRUE(f); std::fclose(f); }
    es_.activeScene().getQuadManager().createQuad("Quad 2");
    es_.pendingFileAction = FileAction::Exit;

    session_->processPendingFileAction();

    EXPECT_EQ(es_.unsavedPromptFor, FileAction::Exit);
    EXPECT_FALSE(exitConfirmed_);
    EXPECT_TRUE(fs::exists(dir_ / "show.autosave.qmap"));
}

// A Load that fails leaves the open show open, so its autosave must stay: autosave
// does not write an unchanged show again.
TEST_F(ProjectSessionTest, AFailedLoadKeepsTheOpenShowsRecoveryFiles) {
    openAs(file("show.qmap"));
    { std::FILE* f = std::fopen(file("show.autosave.qmap").c_str(), "wb"); ASSERT_TRUE(f); std::fclose(f); }
    { std::FILE* f = std::fopen(file("broken.qmap").c_str(), "wb"); ASSERT_TRUE(f); std::fputs("not a show", f); std::fclose(f); }
    es_.pendingLoadPath = file("broken.qmap");
    es_.pendingFileAction = FileAction::Load;

    session_->processPendingFileAction();

    EXPECT_EQ(project_.getFilePath(), file("show.qmap"));
    EXPECT_EQ(replaced_, 0);
    EXPECT_TRUE(fs::exists(dir_ / "show.autosave.qmap"));
}

TEST_F(ProjectSessionTest, ALoadThatSucceedsDiscardsTheLeftShowsRecoveryFiles) {
    openAs(file("show.qmap"));
    es_.pendingFileAction = FileAction::Save;
    session_->processPendingFileAction();
    ASSERT_TRUE(fs::exists(dir_ / "show.qmap"));
    fs::copy_file(dir_ / "show.qmap", dir_ / "other.qmap");
    { std::FILE* f = std::fopen(file("show.autosave.qmap").c_str(), "wb"); ASSERT_TRUE(f); std::fclose(f); }
    es_.pendingLoadPath = file("other.qmap");
    es_.pendingFileAction = FileAction::Load;

    session_->processPendingFileAction();

    EXPECT_EQ(project_.getFilePath(), file("other.qmap"));
    EXPECT_FALSE(fs::exists(dir_ / "show.autosave.qmap"));
}

// While the recovery prompt is unanswered, saving must not delete the file it offers.
TEST_F(ProjectSessionTest, SavingKeepsTheRecoveryFileTheOpenPromptOffers) {
    openAs(file("show.qmap"));
    { std::FILE* f = std::fopen(file("show.autosave.qmap").c_str(), "wb"); ASSERT_TRUE(f); std::fclose(f); }
    es_.recoveryFilePath = file("show.autosave.qmap");
    es_.recoveryProjectPath = file("show.qmap");
    es_.pendingFileAction = FileAction::Save;

    session_->processPendingFileAction();

    EXPECT_TRUE(fs::exists(dir_ / "show.qmap"));
    EXPECT_TRUE(fs::exists(dir_ / "show.autosave.qmap"));
}

TEST_F(ProjectSessionTest, EmergencySaveWritesTheCrashFileNotTheProject) {
    openAs(file("show.qmap"));
    es_.activeScene().getQuadManager().createQuad("Quad 2");

    session_->emergencySave();

    EXPECT_FALSE(fs::exists(dir_ / "show.qmap"));
    auto crash = ProjectSerializer::load(dir_ / "show.crash.qmap");
    ASSERT_TRUE(crash);
    ASSERT_EQ(crash.value().getScenes().size(), 1u);
    EXPECT_EQ(crash.value().getScenes()[0].quads.size(), 2u);
}

// The UI hears of every show that replaced the open one (EditorUi::projectReplaced),
// once, and never of a load that failed.
TEST_F(ProjectSessionTest, ProjectReplacedRunsOnceForEachNewLoadAndRestore) {
    openAs(file("show.qmap"));
    ASSERT_TRUE(ProjectSerializer::save(project_, dir_ / "saved.qmap"));

    es_.pendingFileAction = FileAction::New;
    session_->processPendingFileAction();
    EXPECT_EQ(replaced_, 1) << "New";

    es_.pendingLoadPath = file("saved.qmap");
    es_.pendingFileAction = FileAction::Load;
    session_->processPendingFileAction();
    EXPECT_EQ(replaced_, 2) << "Load";

    es_.recoveryFilePath = file("saved.qmap");
    es_.recoveryProjectPath = file("show.qmap");
    es_.pendingFileAction = FileAction::RestoreRecovery;
    session_->processPendingFileAction();
    EXPECT_EQ(replaced_, 3) << "Restore";

    es_.discardConfirmed = true;  // the restored work is unsaved
    es_.pendingLoadPath = file("missing.qmap");
    es_.pendingFileAction = FileAction::Load;
    session_->processPendingFileAction();
    EXPECT_EQ(replaced_, 3) << "a failed Load replaced nothing";
}

// The Missing Media window closes itself once the show that had missing files is gone:
// after New, nothing is missing (MissingMediaWindow::tick).
TEST_F(ProjectSessionTest, MissingMediaWindowClosesAfterTheShowIsReplaced) {
    openAs(file("show.qmap"));
    Project::SourceData gone;
    gone.id = UUID::generate();
    gone.kind = "video";
    gone.path = file("gone.mp4");
    gone.missing = true;
    es_.missingSources.emplace(gone.id, gone);
    es_.activeScene().getQuadManager().getQuads()[0]->getTexture().mediaId = gone.id;

    MissingMediaWindow window;
    window.visible = true;
    EditorContext ctx{es_, &project_, nullptr, &sources_, nullptr, nullptr, nullptr};
    window.tick(ctx);
    ASSERT_TRUE(window.visible) << "a file is still missing";

    es_.discardConfirmed = true;
    es_.pendingFileAction = FileAction::New;
    session_->processPendingFileAction();
    ASSERT_EQ(replaced_, 1);

    window.tick(ctx);
    EXPECT_FALSE(window.visible);
}

// The perform lock (ADR-051 §4) keeps the show that is on: New, Open, Load, Import and
// Restore are refused, whoever queued them, while Save still writes the show.
TEST_F(ProjectSessionTest, PerformLockRefusesNewAndLoadButSaves) {
    openAs(file("show.qmap"));
    ASSERT_TRUE(ProjectSerializer::save(project_, dir_ / "other.qmap"));
    es_.activeScene().getQuadManager().createQuad("Quad 2");
    lockPerform(es_);

    es_.discardConfirmed = true;  // even with "Don't Save" already answered
    es_.pendingFileAction = FileAction::New;
    session_->processPendingFileAction();
    EXPECT_EQ(quadCount(), 2u) << "New refused";
    EXPECT_EQ(es_.unsavedPromptFor, FileAction::None) << "no prompt for a refused action";
    EXPECT_NE(std::string(es_.statusMessage).find("the show is locked"), std::string::npos)
        << es_.statusMessage;

    es_.pendingLoadPath = file("other.qmap");
    es_.pendingFileAction = FileAction::Load;
    session_->processPendingFileAction();
    EXPECT_EQ(quadCount(), 2u) << "Load refused";
    EXPECT_TRUE(es_.pendingLoadPath.empty());

    for (FileAction action : {FileAction::Open, FileAction::ImportObject}) {
        es_.pendingFileAction = action;
        session_->processPendingFileAction();
        EXPECT_FALSE(session_->isFileDialogBusy()) << "no dialog opened";
    }
    es_.recoveryFilePath = file("other.qmap");
    es_.recoveryProjectPath = file("show.qmap");
    es_.pendingFileAction = FileAction::RestoreRecovery;
    session_->processPendingFileAction();
    EXPECT_EQ(quadCount(), 2u) << "Restore refused";
    EXPECT_EQ(replaced_, 0);
    EXPECT_EQ(takesDropped_, 0);
    es_.recoveryFilePath.clear();

    es_.pendingFileAction = FileAction::Save;
    session_->processPendingFileAction();
    auto saved = ProjectSerializer::load(dir_ / "show.qmap");
    ASSERT_TRUE(saved) << "Save runs under the lock";
    EXPECT_EQ(saved.value().getScenes()[0].quads.size(), 2u);
    EXPECT_FALSE(session_->isProjectDirty());
}

// TD-34: an Unsaved Changes prompt for New opened before locking and answered "Don't
// Save" after it. New is refused, and the answer goes with it: Exit (allowed under the
// lock) still asks about the unsaved changes instead of closing without them.
TEST_F(ProjectSessionTest, ADontSaveAnsweredAfterLockingIsNotKeptForExit) {
    openAs(file("show.qmap"));
    es_.activeScene().getQuadManager().createQuad("Quad 2");  // unsaved
    lockPerform(es_);

    es_.discardConfirmed = true;  // the prompt's "Don't Save"
    es_.pendingFileAction = FileAction::New;
    session_->processPendingFileAction();
    EXPECT_EQ(quadCount(), 2u) << "New refused";

    es_.pendingFileAction = FileAction::Exit;
    session_->processPendingFileAction();
    EXPECT_FALSE(exitConfirmed_) << "closed without asking about the unsaved changes";
    EXPECT_EQ(es_.unsavedPromptFor, FileAction::Exit);

    // "Save" answered after locking does not save before the next Exit by itself.
    es_.unsavedPromptFor = FileAction::None;
    es_.saveBeforeAction = true;
    es_.pendingFileAction = FileAction::New;
    session_->processPendingFileAction();
    EXPECT_FALSE(es_.saveBeforeAction);
}

// ---- Pickers answered after the lock was taken (ADR-051 §4) --------------------------
// The file dialogs are ownerless, so the menu stays usable while one is open, and it
// can be answered after locking. The answer is refused, with a status.

namespace {

std::string writeTriangleObj(const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::FILE* out = std::fopen(utf8FromPath(file).c_str(), "wb");
    if (out) {
        std::fputs("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", out);
        std::fclose(out);
    }
    return utf8FromPath(file);
}

// A saved object whose mesh file is gone, as a load finds it: listed in Missing Media.
void addMissingMesh(EditorState& es, const std::string& path) {
    es.activeScene().createObject("Prop")->setSourcePath(path);
    requestMeshLoads(es);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!es.meshLoads.tickets.empty() && std::chrono::steady_clock::now() < deadline) {
        adoptLoadedMeshes(es);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool statusSaysLocked(const EditorState& es) {
    return std::string(es.statusMessage).find("the show is locked") != std::string::npos;
}

}  // namespace

TEST_F(ProjectSessionTest, ImportObjectPickedAfterLockingAddsNothing) {
    openAs(file("show.qmap"));
    const std::string obj = writeTriangleObj(dir_ / "prop.obj");
    lockPerform(es_);
    session_->objectFileChosen(obj);
    EXPECT_TRUE(es_.activeScene().getObjects().empty());
    EXPECT_TRUE(statusSaysLocked(es_)) << es_.statusMessage;
}

TEST_F(ProjectSessionTest, LocatePickedAfterLockingRelinksNothing) {
    openAs(file("show.qmap"));
    addMissingMesh(es_, utf8FromPath(dir_ / "gone" / "prop.obj"));
    ASSERT_EQ(collectMissingMedia(es_).size(), 1u);
    const MissingMediaItem item = collectMissingMedia(es_)[0];
    const std::string found = writeTriangleObj(dir_ / "found" / "prop.obj");

    lockPerform(es_);
    session_->locateFileChosen(item, found);
    EXPECT_EQ(collectMissingMedia(es_).size(), 1u) << "still missing";
    EXPECT_TRUE(es_.meshLoads.relocations.empty());
    EXPECT_TRUE(statusSaysLocked(es_)) << es_.statusMessage;
}

TEST_F(ProjectSessionTest, SearchPickedAfterLockingStartsNothing) {
    openAs(file("show.qmap"));
    lockPerform(es_);
    session_->searchFolderChosen(utf8FromPath(dir_));
    EXPECT_FALSE(session_->isSearchingMedia());
    EXPECT_TRUE(statusSaysLocked(es_)) << es_.statusMessage;
}

// A search started before locking finishes under the lock: its result waits, and is
// applied once the show is unlocked.
TEST_F(ProjectSessionTest, ASearchFinishingUnderTheLockIsHeldUntilUnlocked) {
    openAs(file("show.qmap"));
    addMissingMesh(es_, utf8FromPath(dir_ / "gone" / "prop.obj"));
    ASSERT_EQ(collectMissingMedia(es_).size(), 1u);
    writeTriangleObj(dir_ / "found" / "prop.obj");

    session_->searchFolderChosen(utf8FromPath(dir_ / "found"));
    ASSERT_TRUE(session_->isSearchingMedia());
    lockPerform(es_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::string(es_.statusMessage).find("once the show is unlocked") == std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
        session_->pollMissingMediaSearch();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_NE(std::string(es_.statusMessage).find("once the show is unlocked"), std::string::npos)
        << es_.statusMessage;
    session_->pollMissingMediaSearch();
    EXPECT_EQ(collectMissingMedia(es_).size(), 1u) << "nothing relinked while locked";
    EXPECT_TRUE(session_->isSearchingMedia()) << "the result is held, not dropped";

    requestPerformUnlock(es_);
    ASSERT_TRUE(confirmPerformUnlock(es_));
    session_->pollMissingMediaSearch();
    EXPECT_FALSE(session_->isSearchingMedia());
    EXPECT_TRUE(collectMissingMedia(es_).empty()) << "relinked after the unlock";
}
