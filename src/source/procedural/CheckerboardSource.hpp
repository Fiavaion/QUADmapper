#pragma once

#include "ProceduralSource.hpp"

namespace quadmapper {

/**
 * @brief Procedural source that generates checkerboard patterns
 */
class CheckerboardSource : public ProceduralSource {
public:
    CheckerboardSource(int width = 1920, int height = 1080);

    // --- TextureSource ---
    SourceType getType() const override { return SourceType::PROCEDURAL_CHECKERBOARD; }
    std::string getTypeName() const override { return "Checkerboard"; }

    // --- ProceduralSource ---
    std::vector<SourceParameter> getParameters() const override;
    void setParameter(const std::string& name, const ParamValue& value) override;
    void resetParameters() override;
    bool isAnimated() const override { return false; }

    // --- CheckerboardSource ---
    void setColor1(const Color& color);
    void setColor2(const Color& color);
    void setSize(float size);

protected:
    std::string getShaderSource() const override;
    void setShaderUniforms() override;

private:
    Color color1_ = Color(0.0f, 0.0f, 0.0f, 1.0f);  // Black
    Color color2_ = Color(1.0f, 1.0f, 1.0f, 1.0f);  // White
    float size_ = 8.0f;  // Number of squares
};

} // namespace quadmapper
