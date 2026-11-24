#pragma once

#include "core/UUID.hpp"
#include "Quad.hpp"
#include <vector>
#include <memory>
#include <unordered_set>
#include <optional>
#include <functional>

namespace quadmapper {

// QuadManager manages a collection of quads with selection support
class QuadManager {
public:
    QuadManager();
    ~QuadManager() = default;

    // Non-copyable, movable
    QuadManager(const QuadManager&) = delete;
    QuadManager& operator=(const QuadManager&) = delete;
    QuadManager(QuadManager&&) = default;
    QuadManager& operator=(QuadManager&&) = default;

    // Quad creation and deletion
    Quad* createQuad();
    Quad* createQuad(const std::string& name);
    void deleteQuad(const UUID& id);
    void clear();

    // Re-insert a quad (keeping its id and state) at list position `index`
    // (clamped), which is also its z-order. Used by undo/redo to restore a
    // deleted quad. Returns nullptr if the quad is null or its id already exists.
    Quad* insertQuad(std::unique_ptr<Quad> quad, size_t index);

    // Quad access
    Quad* getQuad(const UUID& id);
    const Quad* getQuad(const UUID& id) const;
    size_t getQuadCount() const { return quads_.size(); }
    bool isEmpty() const { return quads_.empty(); }

    // Iteration
    const std::vector<std::unique_ptr<Quad>>& getQuads() const { return quads_; }
    std::vector<Quad*> getQuadsByZOrder() const;

    // Topmost VISIBLE quad whose area contains the canvas-space point p (0..1), or
    // nullptr if none. Used for click-to-select and drop-onto-quad in the viewport.
    Quad* quadAtPoint(const Vec2& p) const;

    // Selection - single select
    void selectQuad(const UUID& id);
    void deselectQuad(const UUID& id);
    void deselectAll();
    void toggleSelection(const UUID& id);

    // Selection - multi-select
    void addToSelection(const UUID& id);
    void selectAll();
    void selectRange(const UUID& fromId, const UUID& toId);

    // Selection queries
    bool isSelected(const UUID& id) const;
    bool hasSelection() const { return !selectedIds_.empty(); }
    size_t getSelectionCount() const { return selectedIds_.size(); }
    const std::unordered_set<UUID>& getSelectedIds() const { return selectedIds_; }
    std::vector<Quad*> getSelectedQuads() const;

    // Primary selection (for inspector - last selected quad)
    std::optional<UUID> getPrimarySelectedId() const { return primarySelectedId_; }
    Quad* getPrimarySelectedQuad();
    const Quad* getPrimarySelectedQuad() const;

    // Quad ordering (single quad)
    void moveQuadUp(const UUID& id);
    void moveQuadDown(const UUID& id);
    void moveQuadToTop(const UUID& id);
    void moveQuadToBottom(const UUID& id);

    // Quad ordering (selected quads - maintains relative order within selection)
    void moveSelectedUp();
    void moveSelectedDown();
    void moveSelectedToTop();
    void moveSelectedToBottom();

    // Z-order sorted access (highest z-order first - for UI display)
    std::vector<Quad*> getQuadsByZOrderDescending() const;

    // Duplicate: full-state copy under newId, with a " Copy" name and a small offset,
    // placed on top and selected. Returns nullptr if id is unknown or newId is taken.
    Quad* duplicateQuad(const UUID& id, const UUID& newId = UUID::generate());

    std::string generateUniqueName() const;

    // Callbacks for external notifications
    using SelectionChangedCallback = std::function<void()>;
    void setSelectionChangedCallback(SelectionChangedCallback callback) {
        selectionChangedCallback_ = std::move(callback);
    }

private:
    void notifySelectionChanged();
    int findQuadIndex(const UUID& id) const;
    void updateZOrders();

    std::vector<std::unique_ptr<Quad>> quads_;
    std::unordered_set<UUID> selectedIds_;
    std::optional<UUID> primarySelectedId_;
    int nextQuadNumber_ = 1;

    SelectionChangedCallback selectionChangedCallback_;
};

} // namespace quadmapper
