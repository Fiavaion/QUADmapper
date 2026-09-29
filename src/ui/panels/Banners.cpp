#include "Banners.hpp"

#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "output/OutputManager.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>

namespace quadmapper {

namespace {

void drawBlackoutBanner(const EditorState& es) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool haveCanvas = es.canvasRegionSize.x > 1.0f && es.canvasRegionSize.y > 1.0f;
    // Across the top of the canvas, just below the zoom controls so they stay usable.
    const float top = haveCanvas ? es.canvasRegionPos.y : viewport->WorkPos.y - viewport->Pos.y;
    const ImVec2 origin(viewport->Pos.x + (haveCanvas ? es.canvasRegionPos.x : 0.0f),
                        viewport->Pos.y + top + ImGui::GetFrameHeight() + 16.0f);
    const float width = haveCanvas ? es.canvasRegionSize.x : viewport->WorkSize.x;

    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 2.2f;
    const char* title = "BLACKOUT";
    const char* detail = "every projector output is black  -  Ctrl+B or Output > Blackout to restore";
    const ImVec2 titleSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, title);
    const ImVec2 detailSize = ImGui::CalcTextSize(detail);
    const float height = titleSize.y + detailSize.y + 16.0f;

    ImDrawList* draw = ImGui::GetForegroundDrawList(viewport);
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(200, 20, 20, 235));
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
    draw->AddText(font, size, ImVec2(origin.x + (width - titleSize.x) * 0.5f, origin.y + 4.0f),
                  IM_COL32(255, 255, 255, 255), title);
    draw->AddText(ImVec2(origin.x + (width - detailSize.x) * 0.5f, origin.y + 8.0f + titleSize.y),
                  IM_COL32(255, 255, 255, 255), detail);
}

void drawBlendTestPatternBanner(const EditorState& es) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool haveCanvas = es.canvasRegionSize.x > 1.0f && es.canvasRegionSize.y > 1.0f;
    const char* text = "BLEND TEST PATTERN ON  -  the projectors show the test pattern, not the show";
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float height = textSize.y + 12.0f;
    const float width = haveCanvas ? es.canvasRegionSize.x : viewport->WorkSize.x;
    const float bottom = haveCanvas ? es.canvasRegionPos.y + es.canvasRegionSize.y
                                    : viewport->WorkPos.y - viewport->Pos.y + viewport->WorkSize.y;
    const ImVec2 origin(viewport->Pos.x + (haveCanvas ? es.canvasRegionPos.x : 0.0f),
                        viewport->Pos.y + bottom - height - 8.0f);

    ImDrawList* draw = ImGui::GetForegroundDrawList(viewport);
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(230, 150, 0, 235));
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(0, 0, 0, 255), 0.0f, 0, 2.0f);
    draw->AddText(ImVec2(origin.x + (width - textSize.x) * 0.5f, origin.y + 6.0f),
                  IM_COL32(0, 0, 0, 255), text);
}

// The perform lock (ADR-051 §4): a badge in the canvas's top-right corner, clear of the
// zoom controls (top left) and the BLACKOUT banner below them.
void drawLockedBanner(const EditorState& es) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool haveCanvas = es.canvasRegionSize.x > 1.0f && es.canvasRegionSize.y > 1.0f;
    const float left = haveCanvas ? es.canvasRegionPos.x : 0.0f;
    const float top = haveCanvas ? es.canvasRegionPos.y : viewport->WorkPos.y - viewport->Pos.y;
    const float width = haveCanvas ? es.canvasRegionSize.x : viewport->WorkSize.x;

    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 1.6f;
    const char* title = "LOCKED";
    const char* detail = "show edits are off  -  Output > Perform Lock to unlock";
    const ImVec2 titleSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, title);
    const ImVec2 detailSize = ImGui::CalcTextSize(detail);
    const float boxWidth = std::max(titleSize.x, detailSize.x) + 20.0f;
    const float boxHeight = titleSize.y + detailSize.y + 14.0f;
    const ImVec2 origin(viewport->Pos.x + left + width - boxWidth - 8.0f, viewport->Pos.y + top + 8.0f);
    const ImVec2 end(origin.x + boxWidth, origin.y + boxHeight);

    ImDrawList* draw = ImGui::GetForegroundDrawList(viewport);
    draw->AddRectFilled(origin, end, IM_COL32(20, 90, 200, 235), 4.0f);
    draw->AddRect(origin, end, IM_COL32(255, 255, 255, 255), 4.0f, 0, 2.0f);
    draw->AddText(font, size, ImVec2(origin.x + (boxWidth - titleSize.x) * 0.5f, origin.y + 4.0f),
                  IM_COL32(255, 255, 255, 255), title);
    draw->AddText(ImVec2(origin.x + (boxWidth - detailSize.x) * 0.5f, origin.y + 8.0f + titleSize.y),
                  IM_COL32(255, 255, 255, 255), detail);
}

}  // namespace

void Banners::drawContent(EditorContext& ctx) {
    if (ctx.es.performLock.isOn()) drawLockedBanner(ctx.es);
    if (!ctx.outputManager) return;
    if (ctx.outputManager->isBlackout()) drawBlackoutBanner(ctx.es);
    if (ctx.outputManager->isBlendTestPattern()) drawBlendTestPatternBanner(ctx.es);
}

}  // namespace quadmapper
