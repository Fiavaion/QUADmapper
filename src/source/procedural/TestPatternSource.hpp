#pragma once

#include "ProceduralSource.hpp"

namespace quadmapper {

/**
 * @brief Procedural source that generates test patterns (SMPTE color bars, etc.)
 */
class TestPatternSource : public ProceduralSource {
public:
    enum class PatternType {
        SMPTEBars = 0,
        Grid = 1,
        Gradient = 2,
        Circle = 3
    };

    TestPatternSource(int width = 1920, int height = 1080);

    // --- TextureSource ---
    SourceType getType() const override { return SourceType::PROCEDURAL_TEST_PATTERN; }
    std::string getTypeName() const override { return "Test Pattern"; }

    // --- ProceduralSource ---
    std::vector<SourceParameter> getParameters() const override;
    void setParameter(const std::string& name, const ParamValue& value) override;
    void resetParameters() override;
    bool isAnimated() const override { return false; }

    // --- TestPatternSource ---
    void setPatternType(PatternType type);

protected:
    std::string getShaderSource() const override;
    void setShaderUniforms() override;

private:
    PatternType patternType_ = PatternType::SMPTEBars;
};

} // namespace quadmapper
