#pragma once

#include "ProceduralSource.hpp"

namespace quadmapper {

/**
 * @brief Procedural source that generates gradient textures
 */
class GradientSource : public ProceduralSource {
public:
    enum class GradientType {
        Linear = 0,
        Radial = 1,
        Angular = 2,
        Diamond = 3
    };

    GradientSource(int width = 1920, int height = 1080);

    // --- TextureSource ---
    SourceType getType() const override { return SourceType::PROCEDURAL_GRADIENT; }
    std::string getTypeName() const override { return "Gradient"; }

    // --- ProceduralSource ---
    std::vector<SourceParameter> getParameters() const override;
    void setParameter(const std::string& name, const ParamValue& value) override;
    void resetParameters() override;
    bool isAnimated() const override { return animate_; }

    // --- GradientSource ---
    void setStartColor(const Color& color);
    void setEndColor(const Color& color);
    void setAngle(float degrees);
    void setGradientType(GradientType type);
    void setAnimate(bool animate);

protected:
    std::string getShaderSource() const override;
    void setShaderUniforms() override;
    bool onUpdate(double deltaTime) override;

private:
    Color startColor_ = Color(1.0f, 0.0f, 0.0f, 1.0f);  // Red
    Color endColor_ = Color(0.0f, 0.0f, 1.0f, 1.0f);    // Blue
    float angle_ = 0.0f;            // Degrees
    GradientType gradientType_ = GradientType::Linear;
    bool animate_ = false;
    float animationSpeed_ = 30.0f;  // Degrees per second
};

} // namespace quadmapper
