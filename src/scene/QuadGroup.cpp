#include "QuadGroup.hpp"

namespace quadmapper {

QuadGroup::QuadGroup() : id_(UUID::generate()), name_("New Group") {}

QuadGroup::QuadGroup(const UUID& id) : id_(id), name_("New Group") {}

void QuadGroup::addMember(const UUID& quadId) {
    if (std::find(memberQuadIds_.begin(), memberQuadIds_.end(), quadId) == memberQuadIds_.end()) {
        memberQuadIds_.push_back(quadId);
    }
}

void QuadGroup::removeMember(const UUID& quadId) {
    auto it = std::find(memberQuadIds_.begin(), memberQuadIds_.end(), quadId);
    if (it != memberQuadIds_.end()) {
        memberQuadIds_.erase(it);
    }
}

bool QuadGroup::hasMember(const UUID& quadId) const {
    return std::find(memberQuadIds_.begin(), memberQuadIds_.end(), quadId) != memberQuadIds_.end();
}

} // namespace quadmapper
