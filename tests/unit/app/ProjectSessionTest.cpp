// The show-safe project session (app/ProjectSession.cpp, ADR-046 §4): the order file
// actions settle unsaved changes in, New, Exit and the emergency save, on a real
// EditorState with no outputs, audio or GL. Every test gives the project a file in a
// temp folder first, so no action touches the machine's untitled recovery files.

#include "app/ProjectSession.hpp"
#include "app/EditorState.hpp"
#include "app/ProjectMapping.hpp"
#include "core/FileSystem.hpp"
#include "core/UUID.hpp"
#include "project/Project.hpp"
#include "project/ProjectSerializer.hpp"
#include "source/SourceManager.hpp"
#include "ui/dialogs/PreferencesDialog.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

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
