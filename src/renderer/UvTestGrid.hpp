#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/UUID.hpp"

namespace quadmapper {

class Texture;

// Generate a Blender-style colored UV reference grid as RGBA8 pixels (size x size).
// Each cell is flat-tinted by its (u,v) center so orientation/stretch reads at a
// glance; white lines mark every cell; the four corner cells are pure R/G/B/Y so
// flips and rotation are unmistakable. Pure function (no GL) — unit-testable.
std::vector<unsigned char> generateUvTestGridRGBA(int size, int cells);

// Build a GL texture from generateUvTestGridRGBA(). Must run on the GL/main
// thread. Returns nullptr on failure.
std::unique_ptr<Texture> makeUvTestGridTexture(int size, int cells);

// Assign a fresh UV test grid as quad `id`'s texture, clearing any file-path
// binding. Shared by the inspector button and the "new quads" toggle so both
// behave identically. No-op if texture creation fails.
void assignUvTestGrid(std::unordered_map<UUID, std::unique_ptr<Texture>>& textures,
                      std::unordered_map<UUID, std::string>& paths,
                      const UUID& id, int size, int cells);

}  // namespace quadmapper
