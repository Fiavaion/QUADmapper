#pragma once

#include "UUID.hpp"

#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <unordered_set>

namespace quadmapper {

// Forward declarations
class Command;

// Command interface - base class for all undoable commands
class Command {
public:
    virtual ~Command() = default;

    // Execute the command. Returns false when it could not be applied (e.g. its
    // target quad no longer exists); a failed command must leave state untouched.
    virtual bool execute() = 0;

    // Undo the command. Same failure contract as execute().
    virtual bool undo() = 0;

    // Get a description of this command (for UI display)
    virtual std::string getDescription() const = 0;

    // Whether this command can be merged with another
    // Used for merging similar rapid-fire commands (e.g., dragging)
    virtual bool canMergeWith(const Command& other) const { (void)other; return false; }

    // Merge another command into this one
    // Only called if canMergeWith returns true
    virtual void mergeWith(const Command& other) { (void)other; }

    // Media source ids this command can put back onto a quad (e.g. the snapshot of a
    // deleted quad). They must stay loaded while the command is in the history.
    virtual void collectMediaIds(std::unordered_set<UUID>& out) const { (void)out; }

    // A media source was replaced by another (a missing file relocated): what this
    // command would put back must use the new id.
    virtual void remapMediaId(const UUID& from, const UUID& to) { (void)from; (void)to; }

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

    // Execute a command and add it to history. A command that fails on its first
    // execution is not recorded.
    void execute(std::unique_ptr<Command> command);

    // Undo / redo one step. Returns false when there is nothing to undo/redo or the
    // command could not be applied (its target is gone); a failed step is still
    // consumed so the stack never wedges on a stale entry.
    bool undo();
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

    // Every media source id an undo or redo step could restore (see
    // Command::collectMediaIds), including an open compound group.
    void collectMediaIds(std::unordered_set<UUID>& out) const;

    // Command::remapMediaId on every step, including an open compound group.
    void remapMediaId(const UUID& from, const UUID& to);

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

    bool execute() override;
    bool undo() override;
    std::string getDescription() const override { return description_; }

    // Executes the command immediately; it is kept only if that succeeds.
    void addCommand(std::unique_ptr<Command> command);
    bool isEmpty() const { return commands_.empty(); }
    void collectMediaIds(std::unordered_set<UUID>& out) const override;
    void remapMediaId(const UUID& from, const UUID& to) override;

private:
    std::string description_;
    std::vector<std::unique_ptr<Command>> commands_;
};

} // namespace quadmapper
