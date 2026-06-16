#pragma once

#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "QuadGeometry.hpp"
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

namespace quadmapper {

// Texture binding for a quad
struct TextureBinding {
    UUID mediaId;
    Vec2 uvOffset = Vec2(0.0f);      // Pan/offset texture
    Vec2 uvScale = Vec2(1.0f);       // Zoom/scale texture (centered around 0.5)
    float uvRotation = 0.0f;         // Rotation in radians (centered around 0.5)
    std::optional<Rectangle> cropRegion;
    bool flipH = false;
    bool flipV = false;

    // Reset UV transforms to defaults
    void resetUV() {
        uvOffset = Vec2(0.0f);
        uvScale = Vec2(1.0f);
        uvRotation = 0.0f;
    }
};

// Per-quad color correction (ADR-036 / US-104). Identity defaults render
// byte-identically to no correction. Applied in quad.frag in the order
// RGB gain -> brightness -> contrast -> gamma, before alpha x opacity.
struct ColorCorrection {
    float brightness = 0.0f;        // additive [-1, 1]
    float contrast   = 1.0f;        // multiplier about mid-gray (0.5), [0, 2]
    float gamma      = 1.0f;        // display gamma [0.1, 4], applied as pow(c, 1/gamma)
    Vec3  rgbGain    = Vec3(1.0f);  // per-channel multiply (white balance) [0, 2]

    bool isIdentity() const {
        return brightness == 0.0f && contrast == 1.0f && gamma == 1.0f &&
               rgbGain == Vec3(1.0f);
    }
    void reset() { *this = ColorCorrection{}; }
};

// Quad represents a single projection surface
class Quad {
public:
    Quad();
    explicit Quad(const UUID& id);

    // Getters
    const UUID& getId() const { return id_; }
    const std::string& getName() const { return name_; }
    int getZOrder() const { return zOrder_; }
    bool isVisible() const { return visible_; }
    bool isLocked() const { return locked_; }
    float getOpacity() const { return opacity_; }
    const std::optional<UUID>& getParentGroup() const { return parentGroup_; }
    const QuadGeometry& getGeometry() const { return geometry_; }
    QuadGeometry& getGeometry() { return geometry_; }
    const TextureBinding& getTexture() const { return texture_; }
    TextureBinding& getTexture() { return texture_; }
    const ColorCorrection& getColorCorrection() const { return colorCorrection_; }
    ColorCorrection& getColorCorrection() { return colorCorrection_; }
    const std::vector<UUID>& getOutputAssignments() const { return outputAssignments_; }

    // Setters
    void setName(const std::string& name) { name_ = name; }
    void setZOrder(int z) { zOrder_ = z; }
    void setVisible(bool visible) { visible_ = visible; }
    void setLocked(bool locked) { locked_ = locked; }
    void setOpacity(float opacity) { opacity_ = std::clamp(opacity, 0.0f, 1.0f); }
    void setParentGroup(const std::optional<UUID>& group) { parentGroup_ = group; }
    // Set color correction, clamping each parameter to its valid range (ADR-036).
    void setColorCorrection(const ColorCorrection& cc) {
        colorCorrection_.brightness = std::clamp(cc.brightness, -1.0f, 1.0f);
        colorCorrection_.contrast   = std::clamp(cc.contrast,    0.0f, 2.0f);
        colorCorrection_.gamma      = std::clamp(cc.gamma,       0.1f, 4.0f);
        colorCorrection_.rgbGain    = glm::clamp(cc.rgbGain, Vec3(0.0f), Vec3(2.0f));
    }

    // Output assignment
    void assignToOutput(const UUID& outputId);
    void removeFromOutput(const UUID& outputId);
    bool isAssignedToOutput(const UUID& outputId) const;

private:
    UUID id_;
    std::string name_;
    std::optional<UUID> parentGroup_;
    int zOrder_ = 0;
    bool visible_ = true;
    bool locked_ = false;
    float opacity_ = 1.0f;

    QuadGeometry geometry_;
    TextureBinding texture_;
    ColorCorrection colorCorrection_;
    std::vector<UUID> outputAssignments_;
};

} // namespace quadmapper
