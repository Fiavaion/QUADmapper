#pragma once

#include "core/UUID.hpp"
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

namespace quadmapper {

// QuadGroup represents a collection of quads that can be managed together
// A quad can belong to multiple groups (many-to-many relationship)
class QuadGroup {
public:
    QuadGroup();
    explicit QuadGroup(const UUID& id);

    // Identity
    const UUID& getId() const { return id_; }
    const std::string& getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    // Hierarchy (groups can be nested)
    const std::optional<UUID>& getParentGroup() const { return parentGroup_; }
    void setParentGroup(const std::optional<UUID>& parent) { parentGroup_ = parent; }

    // UI state
    bool isCollapsed() const { return collapsed_; }
    void setCollapsed(bool collapsed) { collapsed_ = collapsed; }

    // Visibility toggle - affects all member quads when applied
    bool isVisible() const { return visible_; }
    void setVisible(bool visible) { visible_ = visible; }

    // Lock toggle - affects all member quads when applied
    bool isLocked() const { return locked_; }
    void setLocked(bool locked) { locked_ = locked; }

    // Member quad management
    const std::vector<UUID>& getMemberQuadIds() const { return memberQuadIds_; }
    void addMember(const UUID& quadId);
    void removeMember(const UUID& quadId);
    bool hasMember(const UUID& quadId) const;
    size_t getMemberCount() const { return memberQuadIds_.size(); }
    bool isEmpty() const { return memberQuadIds_.empty(); }

private:
    UUID id_;
    std::string name_;
    std::optional<UUID> parentGroup_;
    std::vector<UUID> memberQuadIds_;
    bool collapsed_ = false;
    bool visible_ = true;
    bool locked_ = false;
};

} // namespace quadmapper
