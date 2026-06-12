#pragma once

#include "core/Types.hpp"

namespace quadmapper {

struct ProjectSettings {
    IVec2 defaultResolution = IVec2(1920, 1080);
    int gridSize = 32;
    bool snapToGrid = false;
};

} // namespace quadmapper
