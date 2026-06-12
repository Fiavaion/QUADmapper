#pragma once

#include "ProceduralSource.hpp"

namespace quadmapper {

/**
 * @brief Procedural source that generates animated plasma textures
 *
 * Classic plasma effect with configurable colors and animation speed.
 */
class PlasmaSource : public ProceduralSource {
public:
    PlasmaSource(int width = 1920, int height = 1080);

    // --- TextureSource ---
    SourceType getType() const override { return SourceType::PROCEDURAL_PLASMA; }
    std::string getTypeName() const override { return "Plasma"; }

    // --- ProceduralSource ---
    std::vector<SourceParameter> getParameters() const override;
    void setParameter(const std::string& name, const ParamValue& value) override;
    void resetParameters() override;
    bool isAnimated() const override { return true; }

    // --- PlasmaSource ---
    void setSpeed(float speed);
    void setComplexity(float complexity);
    void setColor1(const Color& color);
    void setColor2(const Color& color);
    void setColor3(const Color& color);

protected:
    std::string getShaderSource() const override;
    void setShaderUniforms() override;
    bool onUpdate(double deltaTime) override;

private:
    float speed_ = 1.0f;
    float complexity_ = 3.0f;
    Color color1_ = Color(1.0f, 0.0f, 0.0f, 1.0f);  // Red
    Color color2_ = Color(0.0f, 1.0f, 0.0f, 1.0f);  // Green
    Color color3_ = Color(0.0f, 0.0f, 1.0f, 1.0f);  // Blue
};

} // namespace quadmapper
