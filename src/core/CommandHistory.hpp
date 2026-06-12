#pragma once

#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace quadmapper {

// Forward declarations
class Command;

// Command interface - base class for all undoable commands
class Command {
public:
    virtual ~Command() = default;

    // Execute the command
    virtual void execute() = 0;

    // Undo the command
    virtual void undo() = 0;

    // Get a description of this command (for UI display)
    virtual std::string getDescription() const = 0;

    // Whether this command can be merged with another
    // Used for merging similar rapid-fire commands (e.g., dragging)
    virtual bool canMergeWith(const Command& other) const { (void)other; return false; }

    // Merge another command into this one
    // Only called if canMergeWith returns true
    virtual void mergeWith(const Command& other) { (void)other; }

protected:
    Command() = default;
};

// CommandHistory manages undo/redo stack
class CommandHistory {
public:
    // Default max history size
    static constexpr size_t DEFAULT_MAX_HISTORY = 100;

    explicit CommandHistory(size_t maxHistory = DEFAULT_MAX_HISTORY);
    ~CommandHistory() = default;

    // Non-copyable, movable
    CommandHistory(const CommandHistory&) = delete;
    CommandHistory& operator=(const CommandHistory&) = delete;
    CommandHistory(CommandHistory&&) = default;
    CommandHistory& operator=(CommandHistory&&) = default;

    // Execute a command and add it to history
    void execute(std::unique_ptr<Command> command);

    // Undo the last command
    bool undo();

    // Redo the last undone command
    bool redo();

    // Check if undo/redo is available
    bool canUndo() const { return undoIndex_ > 0; }
    bool canRedo() const { return undoIndex_ < commands_.size(); }

    // Get descriptions for UI
    std::string getUndoDescription() const;
    std::string getRedoDescription() const;

    // Clear all history
    void clear();

    // Get/set max history size
    size_t getMaxHistory() const { return maxHistory_; }
    void setMaxHistory(size_t max);

    // Get current stack sizes
    size_t getUndoCount() const { return undoIndex_; }
    size_t getRedoCount() const { return commands_.size() - undoIndex_; }

    // Callback for when history changes
    using HistoryChangedCallback = std::function<void()>;
    void setHistoryChangedCallback(HistoryChangedCallback callback) {
        historyChangedCallback_ = std::move(callback);
    }

    // Begin/end a compound command group
    // All commands executed between begin and end are treated as one undo step
    void beginCompoundCommand(const std::string& description);
    void endCompoundCommand();
    bool isInCompoundCommand() const { return compoundCommand_ != nullptr; }

private:
    void notifyHistoryChanged();
    void trimHistory();

    std::vector<std::unique_ptr<Command>> commands_;
    size_t undoIndex_ = 0;  // Points to next command to redo
    size_t maxHistory_;

    // Compound command support
    class CompoundCommand;
    std::unique_ptr<CompoundCommand> compoundCommand_;

    HistoryChangedCallback historyChangedCallback_;
};

// CompoundCommand - groups multiple commands as one undo step
class CommandHistory::CompoundCommand : public Command {
public:
    explicit CompoundCommand(const std::string& description);

    void execute() override;
    void undo() override;
    std::string getDescription() const override { return description_; }

    void addCommand(std::unique_ptr<Command> command);
    bool isEmpty() const { return commands_.empty(); }

private:
    std::string description_;
    std::vector<std::unique_ptr<Command>> commands_;
};

} // namespace quadmapper
