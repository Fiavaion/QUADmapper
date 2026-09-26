// Unit tests for the UV test-grid pixel generator (#2 / US "UV map texture").
// generateUvTestGridRGBA is a pure function (no GL), so it is tested headlessly.

#include "renderer/UvTestGrid.hpp"
#include "renderer/MissingMediaLook.hpp"

#include <gtest/gtest.h>

using quadmapper::generateUvTestGridRGBA;

namespace {

// RGBA pixel fetch from the flat buffer.
struct Px { unsigned char r, g, b, a; };
Px at(const std::vector<unsigned char>& px, int size, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * size + x) * 4;
    return {px[i], px[i + 1], px[i + 2], px[i + 3]};
}

}  // namespace

TEST(UvTestGridTest, BufferIsCorrectSizeAndOpaque) {
    const int size = 64;
    auto px = generateUvTestGridRGBA(size, 8);
    ASSERT_EQ(px.size(), static_cast<size_t>(size) * size * 4);
    // Every pixel fully opaque.
    for (size_t i = 3; i < px.size(); i += 4) {
        EXPECT_EQ(px[i], 255);
    }
}

TEST(UvTestGridTest, CornerCellsArePureMarkerColors) {
    const int size = 64;
    const int cells = 8;
    auto px = generateUvTestGridRGBA(size, cells);

    // Sample the center of each corner cell (cell size = size/cells = 8 px).
    const int half = (size / cells) / 2;       // 4
    const int last = size - half - 1;          // far side
    Px tl = at(px, size, half, half);
    Px tr = at(px, size, last, half);
    Px br = at(px, size, last, last);
    Px bl = at(px, size, half, last);

    EXPECT_EQ(tl.r, 255); EXPECT_EQ(tl.g, 0);   EXPECT_EQ(tl.b, 0);    // red
    EXPECT_EQ(tr.r, 0);   EXPECT_EQ(tr.g, 255); EXPECT_EQ(tr.b, 0);    // green
    EXPECT_EQ(br.r, 0);   EXPECT_EQ(br.g, 0);   EXPECT_EQ(br.b, 255);  // blue
    EXPECT_EQ(bl.r, 255); EXPECT_EQ(bl.g, 255); EXPECT_EQ(bl.b, 0);    // yellow
}

TEST(UvTestGridTest, GridLinesAreWhiteAtCellBoundary) {
    const int size = 256;   // 32px cells at 8 divisions → lines land on pixels
    const int cells = 8;
    auto px = generateUvTestGridRGBA(size, cells);

    // x=32 is the cell0/cell1 vertical boundary; y=140 is mid-cell (not a corner)
    // → the pixel is on the vertical grid line, so it must be white.
    Px online = at(px, size, 32, 140);
    EXPECT_EQ(online.r, 255);
    EXPECT_EQ(online.g, 255);
    EXPECT_EQ(online.b, 255);

    // Sanity: grid lines actually exist somewhere in the image.
    int whiteCount = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (px[i] == 255 && px[i + 1] == 255 && px[i + 2] == 255) ++whiteCount;
    }
    EXPECT_GT(whiteCount, 0);
}

TEST(UvTestGridTest, DegenerateArgsDoNotCrash) {
    EXPECT_EQ(generateUvTestGridRGBA(0, 0).size(), 4u);   // clamped to 1x1
    EXPECT_EQ(generateUvTestGridRGBA(2, 1).size(), 16u);  // 2x2 RGBA
}

// ---- Missing-media look (PRD §4.1.8, MissingMediaLook.hpp) --------------------------


using quadmapper::SurfaceDraw;
using quadmapper::SurfaceView;
using quadmapper::chooseSurfaceDraw;

TEST(MissingMediaLookTest, MissingSourceIsBlackOnOutputsAndCheckerInEditor) {
    // Never white mid-show: whether or not some fallback texture exists.
    EXPECT_EQ(chooseSurfaceDraw(false, true, SurfaceView::Output), SurfaceDraw::Black);
    EXPECT_EQ(chooseSurfaceDraw(true, true, SurfaceView::Output), SurfaceDraw::Black);
    EXPECT_EQ(chooseSurfaceDraw(false, true, SurfaceView::Editor), SurfaceDraw::MissingPattern);
    EXPECT_EQ(chooseSurfaceDraw(true, true, SurfaceView::Editor), SurfaceDraw::MissingPattern);
}

TEST(MissingMediaLookTest, PresentMediaIsUnchanged) {
    for (SurfaceView view : {SurfaceView::Editor, SurfaceView::Output}) {
        EXPECT_EQ(chooseSurfaceDraw(true, false, view), SurfaceDraw::Texture);
        EXPECT_EQ(chooseSurfaceDraw(false, false, view), SurfaceDraw::Plain);  // unassigned quad
    }
}

TEST(MissingMediaLookTest, PatternIsAnOpaqueMagentaBlackChecker) {
    const int size = quadmapper::MISSING_PATTERN_SIZE;
    const int cell = quadmapper::MISSING_PATTERN_CELL;
    const auto px = quadmapper::generateMissingPatternRGBA();
    ASSERT_EQ(px.size(), static_cast<size_t>(size) * size * 4);
    const Px topLeft = at(px, size, 0, 0);
    EXPECT_EQ(topLeft.r, 255); EXPECT_EQ(topLeft.g, 0); EXPECT_EQ(topLeft.b, 255);
    const Px nextCell = at(px, size, cell, 0);
    EXPECT_EQ(nextCell.r, 0); EXPECT_EQ(nextCell.g, 0); EXPECT_EQ(nextCell.b, 0);
    const Px diagonal = at(px, size, cell, cell);
    EXPECT_EQ(diagonal.r, 255);
    EXPECT_EQ(at(px, size, cell - 1, cell - 1).r, 255);  // cell edges are exact
    for (size_t i = 3; i < px.size(); i += 4) ASSERT_EQ(px[i], 255);
    EXPECT_TRUE(quadmapper::generateMissingPatternRGBA(0, 8).empty());
    EXPECT_EQ(quadmapper::generateMissingPatternRGBA(4, 0).size(), 4u * 4u * 4u);  // cell clamps to 1
}
