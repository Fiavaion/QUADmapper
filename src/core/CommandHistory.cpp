#include "CommandHistory.hpp"
#include "Logger.hpp"

namespace quadmapper {

// CompoundCommand implementation
CommandHistory::CompoundCommand::CompoundCommand(const std::string& description)
    : description_(description)
{
}

void CommandHistory::CompoundCommand::execute() {
    for (auto& cmd : commands_) {
        cmd->execute();
    }
}

void CommandHistory::CompoundCommand::undo() {
    // Undo in reverse order
    for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) {
        (*it)->undo();
    }
}

void CommandHistory::CompoundCommand::addCommand(std::unique_ptr<Command> command) {
    // Execute immediately when adding to compound command
    command->execute();
    commands_.push_back(std::move(command));
}

// CommandHistory implementation
CommandHistory::CommandHistory(size_t maxHistory)
    : maxHistory_(maxHistory)
{
}

void CommandHistory::execute(std::unique_ptr<Command> command) {
    if (!command) {
        LOG_WARNING("Attempted to execute null command");
        return;
    }

    // If we're building a compound command, add to it instead
    if (compoundCommand_) {
        compoundCommand_->addCommand(std::move(command));
        return;
    }

    // Clear any commands after current position (redo stack)
    if (undoIndex_ < commands_.size()) {
        commands_.erase(commands_.begin() + undoIndex_, commands_.end());
    }

    // Check if we can merge with the last command
    if (!commands_.empty() && undoIndex_ > 0) {
        auto& lastCommand = commands_[undoIndex_ - 1];
        if (lastCommand->canMergeWith(*command)) {
            lastCommand->mergeWith(*command);
            command->execute();
            LOG_TRACE("Merged command: {}", command->getDescription());
            notifyHistoryChanged();
            return;
        }
    }

    // Execute the command
    command->execute();
    LOG_TRACE("Executed command: {}", command->getDescription());

    // Add to history
    commands_.push_back(std::move(command));
    undoIndex_ = commands_.size();

    // Trim if we exceed max history
    trimHistory();

    notifyHistoryChanged();
}

bool CommandHistory::undo() {
    if (!canUndo()) {
        LOG_TRACE("Nothing to undo");
        return false;
    }

    --undoIndex_;
    auto& command = commands_[undoIndex_];
    command->undo();
    LOG_DEBUG("Undo: {}", command->getDescription());

    notifyHistoryChanged();
    return true;
}

bool CommandHistory::redo() {
    if (!canRedo()) {
        LOG_TRACE("Nothing to redo");
        return false;
    }

    auto& command = commands_[undoIndex_];
    command->execute();
    LOG_DEBUG("Redo: {}", command->getDescription());
    ++undoIndex_;

    notifyHistoryChanged();
    return true;
}

std::string CommandHistory::getUndoDescription() const {
    if (!canUndo()) {
        return "";
    }
    return commands_[undoIndex_ - 1]->getDescription();
}

std::string CommandHistory::getRedoDescription() const {
    if (!canRedo()) {
        return "";
    }
    return commands_[undoIndex_]->getDescription();
}

void CommandHistory::clear() {
    commands_.clear();
    undoIndex_ = 0;
    compoundCommand_.reset();
    LOG_DEBUG("Command history cleared");
    notifyHistoryChanged();
}

void CommandHistory::setMaxHistory(size_t max) {
    maxHistory_ = max;
    trimHistory();
}

void CommandHistory::beginCompoundCommand(const std::string& description) {
    if (compoundCommand_) {
        LOG_WARNING("Already in compound command - nested compound commands not supported");
        return;
    }
    compoundCommand_ = std::make_unique<CompoundCommand>(description);
    LOG_TRACE("Begin compound command: {}", description);
}

void CommandHistory::endCompoundCommand() {
    if (!compoundCommand_) {
        LOG_WARNING("Not in compound command");
        return;
    }

    auto compound = std::move(compoundCommand_);

    // Only add if there were actually commands
    if (!compound->isEmpty()) {
        // Clear redo stack
        if (undoIndex_ < commands_.size()) {
            commands_.erase(commands_.begin() + undoIndex_, commands_.end());
        }

        LOG_TRACE("End compound command: {} (added to history)", compound->getDescription());
        commands_.push_back(std::move(compound));
        undoIndex_ = commands_.size();

        trimHistory();
        notifyHistoryChanged();
    } else {
        LOG_TRACE("End compound command: empty, discarded");
    }
}

void CommandHistory::notifyHistoryChanged() {
    if (historyChangedCallback_) {
        historyChangedCallback_();
    }
}

void CommandHistory::trimHistory() {
    if (commands_.size() > maxHistory_) {
        size_t toRemove = commands_.size() - maxHistory_;
        commands_.erase(commands_.begin(), commands_.begin() + toRemove);

        // Adjust index
        if (undoIndex_ > toRemove) {
            undoIndex_ -= toRemove;
        } else {
            undoIndex_ = 0;
        }
    }
}

} // namespace quadmapper
