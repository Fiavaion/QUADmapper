#include "QuadManager.hpp"
#include "Quad.hpp"
#include "QuadGeometry.hpp"
#include "core/Logger.hpp"
#include <algorithm>
#include <sstream>

namespace quadmapper {

QuadManager::QuadManager() = default;

Quad* QuadManager::createQuad() {
    return createQuad(generateUniqueName());
}

Quad* QuadManager::createQuad(const std::string& name) {
    auto quad = std::make_unique<Quad>();
    quad->setName(name);
    // New quads warp linearly so snapped neighbours line up (ADR-058).
    quad->getGeometry().setWarpMode(WarpMode::LINEAR);
    quad->setZOrder(static_cast<int>(quads_.size()));

    Quad* ptr = quad.get();
    quads_.push_back(std::move(quad));

    Logger::debug("Created quad '{}' with ID {}", name, ptr->getId().toString());
    return ptr;
}

void QuadManager::deleteQuad(const UUID& id) {
    auto it = std::find_if(quads_.begin(), quads_.end(),
        [&id](const auto& q) { return q->getId() == id; });

    if (it != quads_.end()) {
        Logger::debug("Deleting quad '{}' with ID {}", (*it)->getName(), id.toString());

        // Remove from selection
        selectedIds_.erase(id);
        if (primarySelectedId_.has_value() && *primarySelectedId_ == id) {
            primarySelectedId_ = firstSelectedInOrder();
        }

        quads_.erase(it);
        updateZOrders();
        notifySelectionChanged();
    }
}

Quad* QuadManager::insertQuad(std::unique_ptr<Quad> quad, size_t index) {
    if (!quad || getQuad(quad->getId()) != nullptr) {
        return nullptr;
    }
    index = std::min(index, quads_.size());
    Quad* ptr = quad.get();
    quads_.insert(quads_.begin() + static_cast<std::ptrdiff_t>(index), std::move(quad));
    updateZOrders();
    Logger::debug("Inserted quad '{}' with ID {} at {}", ptr->getName(), ptr->getId().toString(), index);
    return ptr;
}

void QuadManager::clear() {
    quads_.clear();
    selectedIds_.clear();
    primarySelectedId_.reset();
    nextQuadNumber_ = 1;
    notifySelectionChanged();
}

Quad* QuadManager::getQuad(const UUID& id) {
    auto it = std::find_if(quads_.begin(), quads_.end(),
        [&id](const auto& q) { return q->getId() == id; });
    return it != quads_.end() ? it->get() : nullptr;
}

const Quad* QuadManager::getQuad(const UUID& id) const {
    auto it = std::find_if(quads_.begin(), quads_.end(),
        [&id](const auto& q) { return q->getId() == id; });
    return it != quads_.end() ? it->get() : nullptr;
}

std::vector<Quad*> QuadManager::getQuadsByZOrder() const {
    std::vector<Quad*> sorted;
    sorted.reserve(quads_.size());
    for (const auto& q : quads_) {
        sorted.push_back(q.get());
    }
    std::sort(sorted.begin(), sorted.end(),
        [](const Quad* a, const Quad* b) { return a->getZOrder() < b->getZOrder(); });
    return sorted;
}

Quad* QuadManager::quadAtPoint(const Vec2& p) const {
    // getQuadsByZOrder is back -> front; iterate front-most first so the topmost
    // quad wins. The hit area is the drawn surface (ADR-049): surfaceParamsAt returns
    // (u,v) in [0,1] inside it, (-1,-1) outside.
    auto byZ = getQuadsByZOrder();
    for (auto it = byZ.rbegin(); it != byZ.rend(); ++it) {
        Quad* q = *it;
        if (!q || !q->isVisible()) continue;
        if (q->getGeometry().surfaceParamsAt(p).x >= 0.0f) {
            return q;
        }
    }
    return nullptr;
}

// In list order, not the set's: a set's first element is arbitrary, so the new primary
// would differ from run to run.
std::optional<UUID> QuadManager::firstSelectedInOrder() const {
    for (const auto& quad : quads_) {
        if (selectedIds_.count(quad->getId()) > 0) return quad->getId();
    }
    return std::nullopt;
}

void QuadManager::selectQuad(const UUID& id) {
    if (getQuad(id) == nullptr) {
        return;
    }

    selectedIds_.clear();
    selectedIds_.insert(id);
    primarySelectedId_ = id;
    notifySelectionChanged();
}

void QuadManager::deselectQuad(const UUID& id) {
    if (selectedIds_.erase(id) > 0) {
        if (primarySelectedId_.has_value() && *primarySelectedId_ == id) {
            primarySelectedId_ = firstSelectedInOrder();
        }
        notifySelectionChanged();
    }
}

void QuadManager::deselectAll() {
    if (!selectedIds_.empty()) {
        selectedIds_.clear();
        primarySelectedId_.reset();
        notifySelectionChanged();
    }
}

void QuadManager::toggleSelection(const UUID& id) {
    if (isSelected(id)) {
        deselectQuad(id);
    } else {
        addToSelection(id);
    }
}

void QuadManager::addToSelection(const UUID& id) {
    if (getQuad(id) == nullptr) {
        return;
    }

    if (selectedIds_.insert(id).second) {
        primarySelectedId_ = id;
        notifySelectionChanged();
    }
}

void QuadManager::selectAll() {
    bool changed = false;
    for (const auto& quad : quads_) {
        if (selectedIds_.insert(quad->getId()).second) {
            changed = true;
        }
    }
    if (!quads_.empty() && !primarySelectedId_.has_value()) {
        primarySelectedId_ = quads_.back()->getId();
    }
    if (changed) {
        notifySelectionChanged();
    }
}

void QuadManager::selectRange(const UUID& fromId, const UUID& toId) {
    int fromIndex = findQuadIndex(fromId);
    int toIndex = findQuadIndex(toId);

    if (fromIndex < 0 || toIndex < 0) {
        return;
    }

    int startIdx = std::min(fromIndex, toIndex);
    int endIdx = std::max(fromIndex, toIndex);

    for (int i = startIdx; i <= endIdx; ++i) {
        selectedIds_.insert(quads_[i]->getId());
    }

    primarySelectedId_ = toId;
    notifySelectionChanged();
}

bool QuadManager::isSelected(const UUID& id) const {
    return selectedIds_.find(id) != selectedIds_.end();
}

std::vector<Quad*> QuadManager::getSelectedQuads() const {
    std::vector<Quad*> result;
    result.reserve(selectedIds_.size());

    for (const auto& quad : quads_) {
        if (isSelected(quad->getId())) {
            result.push_back(quad.get());
        }
    }

    return result;
}

Quad* QuadManager::getPrimarySelectedQuad() {
    if (!primarySelectedId_.has_value()) {
        return nullptr;
    }
    return getQuad(*primarySelectedId_);
}

const Quad* QuadManager::getPrimarySelectedQuad() const {
    if (!primarySelectedId_.has_value()) {
        return nullptr;
    }
    return getQuad(*primarySelectedId_);
}

void QuadManager::moveQuadUp(const UUID& id) {
    int index = findQuadIndex(id);
    if (index > 0) {
        std::swap(quads_[index], quads_[index - 1]);
        updateZOrders();
    }
}

void QuadManager::moveQuadDown(const UUID& id) {
    int index = findQuadIndex(id);
    if (index >= 0 && index < static_cast<int>(quads_.size()) - 1) {
        std::swap(quads_[index], quads_[index + 1]);
        updateZOrders();
    }
}

void QuadManager::moveQuadToTop(const UUID& id) {
    int index = findQuadIndex(id);
    if (index > 0) {
        auto quad = std::move(quads_[index]);
        quads_.erase(quads_.begin() + index);
        quads_.insert(quads_.begin(), std::move(quad));
        updateZOrders();
    }
}

void QuadManager::moveQuadToBottom(const UUID& id) {
    int index = findQuadIndex(id);
    if (index >= 0 && index < static_cast<int>(quads_.size()) - 1) {
        auto quad = std::move(quads_[index]);
        quads_.erase(quads_.begin() + index);
        quads_.push_back(std::move(quad));
        updateZOrders();
    }
}

void QuadManager::moveSelectedUp() {
    // "Up" in UI (with descending z-order list) = higher z-order = higher array index
    // This brings quads forward (rendered later = on top)
    if (selectedIds_.empty()) return;

    // Get selected quads sorted by index descending (process from end first)
    std::vector<std::pair<int, UUID>> selectedWithIndex;
    for (const auto& id : selectedIds_) {
        int index = findQuadIndex(id);
        if (index >= 0) {
            selectedWithIndex.push_back({index, id});
        }
    }
    std::sort(selectedWithIndex.begin(), selectedWithIndex.end(), std::greater<>());

    bool moved = false;
    for (const auto& [origIndex, id] : selectedWithIndex) {
        int currentIndex = findQuadIndex(id);
        if (currentIndex >= 0 && currentIndex < static_cast<int>(quads_.size()) - 1 &&
            !isSelected(quads_[currentIndex + 1]->getId())) {
            std::swap(quads_[currentIndex], quads_[currentIndex + 1]);
            moved = true;
        }
    }
    if (moved) {
        updateZOrders();
    }
}

void QuadManager::moveSelectedDown() {
    // "Down" in UI (with descending z-order list) = lower z-order = lower array index
    // This sends quads backward (rendered earlier = behind)
    if (selectedIds_.empty()) return;

    // Get selected quads sorted by index ascending (process from start first)
    std::vector<std::pair<int, UUID>> selectedWithIndex;
    for (const auto& id : selectedIds_) {
        int index = findQuadIndex(id);
        if (index >= 0) {
            selectedWithIndex.push_back({index, id});
        }
    }
    std::sort(selectedWithIndex.begin(), selectedWithIndex.end());

    bool moved = false;
    for (const auto& [origIndex, id] : selectedWithIndex) {
        int currentIndex = findQuadIndex(id);
        if (currentIndex > 0 && !isSelected(quads_[currentIndex - 1]->getId())) {
            std::swap(quads_[currentIndex], quads_[currentIndex - 1]);
            moved = true;
        }
    }
    if (moved) {
        updateZOrders();
    }
}

void QuadManager::moveSelectedToTop() {
    // Move to highest z-order = end of array = rendered on top
    if (selectedIds_.empty()) return;

    // Get selected quads sorted by current z-order (ascending) to maintain relative order
    std::vector<std::pair<int, UUID>> selectedWithIndex;
    for (const auto& id : selectedIds_) {
        int index = findQuadIndex(id);
        if (index >= 0) {
            selectedWithIndex.push_back({index, id});
        }
    }
    std::sort(selectedWithIndex.begin(), selectedWithIndex.end());

    // Extract selected quads in reverse index order (so indices remain valid during removal)
    std::vector<std::unique_ptr<Quad>> selectedQuads;
    for (auto it = selectedWithIndex.rbegin(); it != selectedWithIndex.rend(); ++it) {
        int currentIndex = findQuadIndex(it->second);
        if (currentIndex >= 0) {
            selectedQuads.push_back(std::move(quads_[currentIndex]));
            quads_.erase(quads_.begin() + currentIndex);
        }
    }

    // Reverse to restore original relative order, then append to end
    std::reverse(selectedQuads.begin(), selectedQuads.end());
    for (auto& quad : selectedQuads) {
        quads_.push_back(std::move(quad));
    }

    updateZOrders();
}

void QuadManager::moveSelectedToBottom() {
    // Move to lowest z-order = start of array = rendered behind
    if (selectedIds_.empty()) return;

    // Get selected quads sorted by current z-order (ascending) to maintain relative order
    std::vector<std::pair<int, UUID>> selectedWithIndex;
    for (const auto& id : selectedIds_) {
        int index = findQuadIndex(id);
        if (index >= 0) {
            selectedWithIndex.push_back({index, id});
        }
    }
    std::sort(selectedWithIndex.begin(), selectedWithIndex.end());

    // Extract selected quads in reverse index order (so indices remain valid during removal)
    std::vector<std::unique_ptr<Quad>> selectedQuads;
    for (auto it = selectedWithIndex.rbegin(); it != selectedWithIndex.rend(); ++it) {
        int currentIndex = findQuadIndex(it->second);
        if (currentIndex >= 0) {
            selectedQuads.push_back(std::move(quads_[currentIndex]));
            quads_.erase(quads_.begin() + currentIndex);
        }
    }

    // Reverse to restore original relative order, then insert at beginning
    std::reverse(selectedQuads.begin(), selectedQuads.end());
    for (auto it = selectedQuads.rbegin(); it != selectedQuads.rend(); ++it) {
        quads_.insert(quads_.begin(), std::move(*it));
    }

    updateZOrders();
}

std::vector<Quad*> QuadManager::getQuadsByZOrderDescending() const {
    std::vector<Quad*> sorted;
    sorted.reserve(quads_.size());
    for (const auto& q : quads_) {
        sorted.push_back(q.get());
    }
    std::sort(sorted.begin(), sorted.end(),
        [](const Quad* a, const Quad* b) { return a->getZOrder() > b->getZOrder(); });
    return sorted;
}

Quad* QuadManager::duplicateQuad(const UUID& id, const UUID& newId) {
    const Quad* source = getQuad(id);
    if (!source || getQuad(newId) != nullptr) {
        return nullptr;
    }

    auto newQuad = std::make_unique<Quad>(source->cloneWithId(newId));
    newQuad->setName(source->getName() + " Copy");
    newQuad->setZOrder(static_cast<int>(quads_.size()));

    // Offset position slightly so it's visible. Mesh and bezier offsets are
    // relative to the corners, so they follow.
    auto corners = newQuad->getGeometry().getCorners();
    for (auto& corner : corners) {
        corner.x += 0.05f;
        corner.y += 0.05f;
    }
    newQuad->getGeometry().setCorners(corners);

    Quad* ptr = newQuad.get();
    quads_.push_back(std::move(newQuad));

    // Select the new quad
    selectQuad(ptr->getId());

    Logger::debug("Duplicated quad '{}' to '{}'", source->getName(), ptr->getName());
    return ptr;
}

std::string QuadManager::generateUniqueName() const {
    int number = static_cast<int>(quads_.size()) + 1;
    std::string name;

    // Find a unique name
    bool unique = false;
    while (!unique) {
        std::ostringstream oss;
        oss << "Quad " << number;
        name = oss.str();

        unique = true;
        for (const auto& quad : quads_) {
            if (quad->getName() == name) {
                unique = false;
                ++number;
                break;
            }
        }
    }

    return name;
}

void QuadManager::notifySelectionChanged() {
    if (selectionChangedCallback_) {
        selectionChangedCallback_();
    }
}

int QuadManager::findQuadIndex(const UUID& id) const {
    for (size_t i = 0; i < quads_.size(); ++i) {
        if (quads_[i]->getId() == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void QuadManager::updateZOrders() {
    for (size_t i = 0; i < quads_.size(); ++i) {
        quads_[i]->setZOrder(static_cast<int>(i));
    }
}

} // namespace quadmapper
