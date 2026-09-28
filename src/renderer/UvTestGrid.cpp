#include "UvTestGrid.hpp"

#include "Texture.hpp"

#include <algorithm>
#include <cmath>

namespace quadmapper {

std::vector<unsigned char> generateUvTestGridRGBA(int size, int cells) {
    if (size < 1) size = 1;
    if (cells < 1) cells = 1;

    std::vector<unsigned char> px(static_cast<size_t>(size) * size * 4);
    const float lineFrac = 0.04f;  // grid-line thickness as a fraction of a cell

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = (x + 0.5f) / size;
            const float v = (y + 0.5f) / size;
            const int cu = std::min(cells - 1, static_cast<int>(u * cells));
            const int cv = std::min(cells - 1, static_cast<int>(v * cells));

            // Flat per-cell tint from the cell-center UV (red grows with u, green
            // with v) — the classic UV-debug gradient, quantized per cell.
            unsigned char r = static_cast<unsigned char>(((cu + 0.5f) / cells) * 255.0f);
            unsigned char g = static_cast<unsigned char>(((cv + 0.5f) / cells) * 255.0f);
            unsigned char b = 128;

            // Corner cells: pure colors so flips/rotation are unmistakable.
            const bool left = (cu == 0), right = (cu == cells - 1);
            const bool top = (cv == 0), bottom = (cv == cells - 1);
            if (top && left)          { r = 255; g = 0;   b = 0;   }  // TL red
            else if (top && right)    { r = 0;   g = 255; b = 0;   }  // TR green
            else if (bottom && right) { r = 0;   g = 0;   b = 255; }  // BR blue
            else if (bottom && left)  { r = 255; g = 255; b = 0;   }  // BL yellow

            // White grid lines at cell boundaries.
            const float fu = u * cells - std::floor(u * cells);
            const float fv = v * cells - std::floor(v * cells);
            if (fu < lineFrac || fu > 1.0f - lineFrac ||
                fv < lineFrac || fv > 1.0f - lineFrac) {
                r = g = b = 255;
            }

            const size_t i = (static_cast<size_t>(y) * size + x) * 4;
            px[i + 0] = r;
            px[i + 1] = g;
            px[i + 2] = b;
            px[i + 3] = 255;
        }
    }
    return px;
}

std::unique_ptr<Texture> makeUvTestGridTexture(int size, int cells) {
    const auto px = generateUvTestGridRGBA(size, cells);
    auto tex = std::make_unique<Texture>();
    if (!tex->create(size, size, 4, px.data())) {
        return nullptr;
    }
    return tex;
}

void assignUvTestGrid(std::unordered_map<UUID, std::unique_ptr<Texture>>& textures,
                      std::unordered_map<UUID, std::string>& paths,
                      const UUID& id, int size, int cells) {
    if (auto tex = makeUvTestGridTexture(size, cells)) {
        textures[id] = std::move(tex);
        paths.erase(id);
    }
}

}  // namespace quadmapper
