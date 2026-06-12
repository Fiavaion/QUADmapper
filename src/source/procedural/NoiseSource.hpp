#pragma once

#include "ProceduralSource.hpp"

namespace quadmapper {

/**
 * @brief Procedural source that generates animated noise textures
 *
 * Supports Simplex noise with configurable scale, octaves, and animation.
 */
class NoiseSource : public ProceduralSource {
public:
    NoiseSource(int width = 1920, int height = 1080);

    // --- TextureSource ---
    SourceType getType() const override { return SourceType::PROCEDURAL_NOISE; }
    std::string getTypeName() const override { return "Noise"; }

    // --- ProceduralSource ---
    std::vector<SourceParameter> getParameters() const override;
    void setParameter(const std::string& name, const ParamValue& value) override;
    void resetParameters() override;
    bool isAnimated() const override { return animate_; }

    // --- NoiseSource ---
    void setScale(float scale);
    void setOctaves(int octaves);
    void setSpeed(float speed);
    void setAnimate(bool animate);
    void setColor1(const Color& color);
    void setColor2(const Color& color);

protected:
    std::string getShaderSource() const override;
    void setShaderUniforms() override;
    bool onUpdate(double deltaTime) override;

private:
    float scale_ = 4.0f;
    int octaves_ = 4;
    float speed_ = 0.5f;
    bool animate_ = true;
    Color color1_ = Color(0.0f, 0.0f, 0.0f, 1.0f);  // Black
    Color color2_ = Color(1.0f, 1.0f, 1.0f, 1.0f);  // White
};

} // namespace quadmapper
