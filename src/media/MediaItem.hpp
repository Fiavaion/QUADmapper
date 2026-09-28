#pragma once
#include "core/FileSystem.hpp"
#include "core/UUID.hpp"
#include "core/Types.hpp"
#include <string>
#include <filesystem>

namespace quadmapper {

/**
 * @brief Represents a media item in the media library
 *
 * MediaItem is a lightweight reference to content that can be applied
 * to quads. It links to a TextureSource via sourceId_ which handles
 * the actual texture data and updates.
 */
class MediaItem {
public:
    MediaItem();

    // --- Identity ---
    const UUID& getId() const { return id_; }

    // --- Name ---
    const std::string& getName() const { return name_; }
    void setName(const std::string& n) { name_ = n; }

    // --- File Path (for file-based items) ---
    const std::filesystem::path& getPath() const { return path_; }
    void setPath(const std::filesystem::path& p) { path_ = p; name_ = utf8FromPath(p.filename()); }
    bool hasPath() const { return !path_.empty(); }

    // --- Media Type ---
    MediaType getType() const { return type_; }
    void setType(MediaType t) { type_ = t; }

    // --- Source Link ---
    /**
     * @brief Get the linked TextureSource ID
     *
     * This links to the actual TextureSource in SourceManager that
     * provides texture data for rendering.
     */
    const UUID& getSourceId() const { return sourceId_; }
    void setSourceId(const UUID& id) { sourceId_ = id; }
    bool hasSource() const { return !sourceId_.isNull(); }

    // --- Procedural Type (for procedural items) ---
    ProceduralType getProceduralType() const { return proceduralType_; }
    void setProceduralType(ProceduralType t) { proceduralType_ = t; }

private:
    UUID id_;
    std::string name_;
    std::filesystem::path path_;
    MediaType type_ = MediaType::IMAGE;
    UUID sourceId_;                              // Link to TextureSource
    ProceduralType proceduralType_ = ProceduralType::SOLID_COLOR;
};

} // namespace quadmapper
