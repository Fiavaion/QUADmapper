#pragma once

#include "ProceduralSource.hpp"
#include "core/Types.hpp"
#include <memory>
#include <string>
#include <vector>

namespace quadmapper {

/**
 * @brief Factory for creating procedural texture sources
 */
class ProceduralFactory {
public:
    /**
     * @brief Create a procedural source of the specified type
     * @param type The type of procedural texture
     * @param width Texture width (default 1920)
     * @param height Texture height (default 1080)
     * @return Created source, or nullptr on failure
     */
    static std::unique_ptr<ProceduralSource> create(
        ProceduralType type,
        int width = 1920,
        int height = 1080
    );

    /**
     * @brief Get list of available procedural types
     */
    static std::vector<ProceduralType> getAvailableTypes();

    /**
     * @brief Get human-readable name for a procedural type
     */
    static std::string getTypeName(ProceduralType type);

    /**
     * @brief Get description for a procedural type
     */
    static std::string getTypeDescription(ProceduralType type);

    /**
     * @brief Check if a type produces animated output
     */
    static bool isAnimated(ProceduralType type);
};

} // namespace quadmapper
