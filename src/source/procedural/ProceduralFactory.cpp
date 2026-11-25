#include "ProceduralFactory.hpp"
#include "SolidColorSource.hpp"
#include "GradientSource.hpp"
#include "CheckerboardSource.hpp"
#include "TestPatternSource.hpp"
#include "NoiseSource.hpp"
#include "PlasmaSource.hpp"

namespace quadmapper {

std::unique_ptr<ProceduralSource> ProceduralFactory::create(
    ProceduralType type,
    int width,
    int height)
{
    switch (type) {
        case ProceduralType::SOLID_COLOR:
            return std::make_unique<SolidColorSource>(width, height);

        case ProceduralType::GRADIENT:
            return std::make_unique<GradientSource>(width, height);

        case ProceduralType::CHECKERBOARD:
            return std::make_unique<CheckerboardSource>(width, height);

        case ProceduralType::TEST_PATTERN:
            return std::make_unique<TestPatternSource>(width, height);

        case ProceduralType::NOISE:
            return std::make_unique<NoiseSource>(width, height);

        case ProceduralType::PLASMA:
            return std::make_unique<PlasmaSource>(width, height);

        case ProceduralType::GRID:
            // Grid is a special case of TestPattern
            {
                auto source = std::make_unique<TestPatternSource>(width, height);
                source->setPatternType(TestPatternSource::PatternType::Grid);
                return source;
            }

        default:
            return nullptr;
    }
}

std::vector<ProceduralType> ProceduralFactory::getAvailableTypes() {
    return {
        ProceduralType::SOLID_COLOR,
        ProceduralType::GRADIENT,
        ProceduralType::CHECKERBOARD,
        ProceduralType::TEST_PATTERN,
        ProceduralType::NOISE,
        ProceduralType::PLASMA,
        ProceduralType::GRID
    };
}

std::string ProceduralFactory::getTypeName(ProceduralType type) {
    switch (type) {
        case ProceduralType::SOLID_COLOR:   return "Solid Color";
        case ProceduralType::GRADIENT:      return "Gradient";
        case ProceduralType::CHECKERBOARD:  return "Checkerboard";
        case ProceduralType::TEST_PATTERN:  return "Test Pattern";
        case ProceduralType::NOISE:         return "Noise";
        case ProceduralType::PLASMA:        return "Plasma";
        case ProceduralType::GRID:          return "Grid";
        default:                            return "Unknown";
    }
}

bool ProceduralFactory::isAnimated(ProceduralType type) {
    switch (type) {
        case ProceduralType::NOISE:
        case ProceduralType::PLASMA:
            return true;
        default:
            return false;
    }
}

} // namespace quadmapper
