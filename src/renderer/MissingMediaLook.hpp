#pragma once

// How a surface whose media file is missing is drawn (PRD §4.1.8). On a projector it
// must never flash white mid-show, so it is drawn black. In the editor canvas and the
// output previews it gets an unmistakable magenta/black checker so the operator sees
// at a glance which quads still need their file. Pure (no GL), so it is unit-tested.

#include <vector>

namespace quadmapper {

enum class SurfaceView { Editor, Output };               // who is looking
enum class SurfaceDraw { Texture, Plain, MissingPattern, Black };

// hasTexture: a live texture was resolved for the surface. sourceMissing: it is bound
// to a media file that could not be opened (a missing binding wins over any texture).
inline SurfaceDraw chooseSurfaceDraw(bool hasTexture, bool sourceMissing, SurfaceView view) {
    if (sourceMissing) {
        return view == SurfaceView::Output ? SurfaceDraw::Black : SurfaceDraw::MissingPattern;
    }
    return hasTexture ? SurfaceDraw::Texture : SurfaceDraw::Plain;
}

inline constexpr int MISSING_PATTERN_SIZE = 64;  // texels per side
inline constexpr int MISSING_PATTERN_CELL = 8;   // texels per checker cell

// RGBA8 magenta/black checker, size x size, cell texels per square; the top-left cell
// is magenta.
inline std::vector<unsigned char> generateMissingPatternRGBA(int size = MISSING_PATTERN_SIZE,
                                                             int cell = MISSING_PATTERN_CELL) {
    if (size <= 0) return {};
    if (cell <= 0) cell = 1;
    std::vector<unsigned char> px(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool magenta = ((x / cell) + (y / cell)) % 2 == 0;
            unsigned char* p = &px[(static_cast<size_t>(y) * size + x) * 4];
            p[0] = magenta ? 255 : 0;
            p[1] = 0;
            p[2] = magenta ? 255 : 0;
            p[3] = 255;
        }
    }
    return px;
}

}  // namespace quadmapper
