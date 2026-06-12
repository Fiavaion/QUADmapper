#include "Quad.hpp"
#include <algorithm>

namespace quadmapper {

Quad::Quad() : id_(UUID::generate()), name_("New Quad") {}

Quad::Quad(const UUID& id) : id_(id), name_("New Quad") {}

void Quad::assignToOutput(const UUID& outputId) {
    if (!isAssignedToOutput(outputId)) {
        outputAssignments_.push_back(outputId);
    }
}

void Quad::removeFromOutput(const UUID& outputId) {
    auto it = std::find(outputAssignments_.begin(), outputAssignments_.end(), outputId);
    if (it != outputAssignments_.end()) {
        outputAssignments_.erase(it);
    }
}

bool Quad::isAssignedToOutput(const UUID& outputId) const {
    return std::find(outputAssignments_.begin(), outputAssignments_.end(), outputId)
           != outputAssignments_.end();
}

} // namespace quadmapper
