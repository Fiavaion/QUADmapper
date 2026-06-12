#pragma once

#include "ProceduralSource.hpp"

namespace quadmapper {

/**
 * @brief Procedural source that generates a solid color texture
 */
class SolidColorSource : public ProceduralSource {
public:
    SolidColorSource(int width = 1920, int height = 1080);

    // --- TextureSource ---
    SourceType getType() const override { return SourceType::PROCEDURAL_SOLID; }
    std::string getTypeName() const override { return "Solid Color"; }

    // --- ProceduralSource ---
    std::vector<SourceParameter> getParameters() const override;
    void setParameter(const std::string& name, const ParamValue& value) override;
    void resetParameters() override;
    bool isAnimated() const override { return false; }

    // --- SolidColorSource ---
    void setColor(const Color& color);
    const Color& getColor() const { return color_; }

protected:
    std::string getShaderSource() const override;
    void setShaderUniforms() override;

private:
    Color color_ = Color(1.0f, 1.0f, 1.0f, 1.0f);  // White default
};

} // namespace quadmapper
