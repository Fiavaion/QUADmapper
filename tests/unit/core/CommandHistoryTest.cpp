#include <gtest/gtest.h>
#include "core/CommandHistory.hpp"
#include <string>

namespace quadmapper {
namespace test {

// Test command that tracks execution count
class TestCommand : public Command {
public:
    TestCommand(int& executeCount, int& undoCount, const std::string& desc = "Test")
        : executeCount_(executeCount)
        , undoCount_(undoCount)
        , description_(desc)
    {}

    bool execute() override {
        ++executeCount_;
        return true;
    }

    bool undo() override {
        ++undoCount_;
        return true;
    }

    std::string getDescription() const override {
        return description_;
    }

private:
    int& executeCount_;
    int& undoCount_;
    std::string description_;
};

// Mergeable test command
class MergeableCommand : public Command {
public:
    MergeableCommand(int& value, int delta)
        : value_(value)
        , delta_(delta)
        , totalDelta_(delta)
    {}

    bool execute() override {
        value_ += delta_;
        return true;
    }

    bool undo() override {
        value_ -= totalDelta_;
        return true;
    }

    std::string getDescription() const override {
        return "Mergeable";
    }

    bool canMergeWith(const Command& other) const override {
        return dynamic_cast<const MergeableCommand*>(&other) != nullptr;
    }

    void mergeWith(const Command& other) override {
        auto* m = dynamic_cast<const MergeableCommand*>(&other);
        if (m) {
            totalDelta_ += m->delta_;
        }
    }

private:
    int& value_;
    int delta_;
    int totalDelta_;
};

class CommandHistoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        history_ = std::make_unique<CommandHistory>();
        executeCount_ = 0;
        undoCount_ = 0;
    }

    std::unique_ptr<CommandHistory> history_;
    int executeCount_ = 0;
    int undoCount_ = 0;
};

TEST_F(CommandHistoryTest, InitialStateIsEmpty) {
    EXPECT_FALSE(history_->canUndo());
    EXPECT_FALSE(history_->canRedo());
    EXPECT_EQ(history_->getUndoCount(), 0);
    EXPECT_EQ(history_->getRedoCount(), 0);
}

TEST_F(CommandHistoryTest, ExecuteCommandAddsToHistory) {
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_));

    EXPECT_EQ(executeCount_, 1);
    EXPECT_TRUE(history_->canUndo());
    EXPECT_FALSE(history_->canRedo());
    EXPECT_EQ(history_->getUndoCount(), 1);
}

TEST_F(CommandHistoryTest, UndoRevertsCommand) {
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_));
    EXPECT_EQ(executeCount_, 1);

    bool result = history_->undo();

    EXPECT_TRUE(result);
    EXPECT_EQ(undoCount_, 1);
    EXPECT_FALSE(history_->canUndo());
    EXPECT_TRUE(history_->canRedo());
}

TEST_F(CommandHistoryTest, RedoReappliesCommand) {
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_));
    history_->undo();

    bool result = history_->redo();

    EXPECT_TRUE(result);
    EXPECT_EQ(executeCount_, 2);  // Original + redo
    EXPECT_TRUE(history_->canUndo());
    EXPECT_FALSE(history_->canRedo());
}

TEST_F(CommandHistoryTest, UndoRedoSequence) {
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_, "First"));
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_, "Second"));
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_, "Third"));

    EXPECT_EQ(executeCount_, 3);
    EXPECT_EQ(history_->getUndoCount(), 3);

    history_->undo();  // Undo Third
    EXPECT_EQ(undoCount_, 1);
    EXPECT_EQ(history_->getRedoCount(), 1);

    history_->undo();  // Undo Second
    EXPECT_EQ(undoCount_, 2);

    history_->redo();  // Redo Second
    EXPECT_EQ(executeCount_, 4);

    history_->undo();  // Undo Second again
    history_->undo();  // Undo First
    EXPECT_FALSE(history_->canUndo());
    EXPECT_EQ(history_->getRedoCount(), 3);
}

TEST_F(CommandHistoryTest, ExecuteAfterUndoClearsRedoStack) {
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_, "First"));
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_, "Second"));
    history_->undo();

    EXPECT_TRUE(history_->canRedo());

    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_, "Third"));

    EXPECT_FALSE(history_->canRedo());
    EXPECT_EQ(history_->getUndoCount(), 2);  // First and Third
}

TEST_F(CommandHistoryTest, GetUndoDescription) {
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_, "My Command"));

    EXPECT_EQ(history_->getUndoDescription(), "My Command");
}

TEST_F(CommandHistoryTest, GetRedoDescription) {
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_, "My Command"));
    history_->undo();

    EXPECT_EQ(history_->getRedoDescription(), "My Command");
}

TEST_F(CommandHistoryTest, ClearRemovesAllHistory) {
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_));
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_));

    history_->clear();

    EXPECT_FALSE(history_->canUndo());
    EXPECT_FALSE(history_->canRedo());
    EXPECT_EQ(history_->getUndoCount(), 0);
}

TEST_F(CommandHistoryTest, MaxHistoryLimitsStack) {
    CommandHistory limitedHistory(3);

    for (int i = 0; i < 5; ++i) {
        limitedHistory.execute(std::make_unique<TestCommand>(executeCount_, undoCount_));
    }

    EXPECT_EQ(limitedHistory.getUndoCount(), 3);
}

TEST_F(CommandHistoryTest, MergeableCommandsMerge) {
    int value = 0;

    history_->execute(std::make_unique<MergeableCommand>(value, 5));
    EXPECT_EQ(value, 5);

    history_->execute(std::make_unique<MergeableCommand>(value, 3));
    EXPECT_EQ(value, 8);

    // Should have merged, so only one undo needed
    history_->undo();
    EXPECT_EQ(value, 0);
}

TEST_F(CommandHistoryTest, CompoundCommandGroupsOperations) {
    history_->beginCompoundCommand("Compound");
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_, "First"));
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_, "Second"));
    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_, "Third"));
    history_->endCompoundCommand();

    EXPECT_EQ(executeCount_, 3);
    EXPECT_EQ(history_->getUndoCount(), 1);  // One compound command

    history_->undo();
    EXPECT_EQ(undoCount_, 3);  // All three undone
}

TEST_F(CommandHistoryTest, EmptyCompoundCommandNotAdded) {
    history_->beginCompoundCommand("Empty");
    history_->endCompoundCommand();

    EXPECT_EQ(history_->getUndoCount(), 0);
}

TEST_F(CommandHistoryTest, UndoReturnsFalseWhenEmpty) {
    EXPECT_FALSE(history_->undo());
}

TEST_F(CommandHistoryTest, RedoReturnsFalseWhenEmpty) {
    EXPECT_FALSE(history_->redo());
}

TEST_F(CommandHistoryTest, NullCommandIgnored) {
    history_->execute(nullptr);
    EXPECT_EQ(history_->getUndoCount(), 0);
}

TEST_F(CommandHistoryTest, HistoryChangedCallbackCalled) {
    int callbackCount = 0;
    history_->setHistoryChangedCallback([&callbackCount]() {
        ++callbackCount;
    });

    history_->execute(std::make_unique<TestCommand>(executeCount_, undoCount_));
    EXPECT_EQ(callbackCount, 1);

    history_->undo();
    EXPECT_EQ(callbackCount, 2);

    history_->redo();
    EXPECT_EQ(callbackCount, 3);

    history_->clear();
    EXPECT_EQ(callbackCount, 4);
}

} // namespace test
} // namespace quadmapper
