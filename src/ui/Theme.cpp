#include "Theme.hpp"
#include "core/FileSystem.hpp"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace quadmapper {

// Static member initialization
Theme::Fonts Theme::s_fonts;

//=============================================================================
// MAIN THEME APPLICATION
//=============================================================================

void Theme::apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    applyToStyle(style);
}

void Theme::applyToStyle(ImGuiStyle& style) {
    configureStyle(style);
    configureColors(style);
}

void Theme::configureStyle(ImGuiStyle& style) {
    // Padding
    style.WindowPadding = ImVec2(Metrics::WindowPadding, Metrics::WindowPadding);
    style.FramePadding = ImVec2(Metrics::FramePaddingX, Metrics::FramePaddingY);
    style.CellPadding = ImVec2(Metrics::CellPadding, Metrics::CellPadding);

    // Spacing
    style.ItemSpacing = ImVec2(Metrics::ItemSpacingX, Metrics::ItemSpacingY);
    style.ItemInnerSpacing = ImVec2(Metrics::ItemInnerSpacing, Metrics::ItemInnerSpacing);
    style.IndentSpacing = Metrics::IndentSpacing;
    style.ColumnsMinSpacing = Metrics::ColumnsMinSpacing;

    // Sizing
    style.ScrollbarSize = Metrics::ScrollbarSize;
    style.GrabMinSize = Metrics::GrabMinSize;

    // Rounding
    style.WindowRounding = Metrics::WindowRounding;
    style.ChildRounding = Metrics::ChildRounding;
    style.FrameRounding = Metrics::FrameRounding;
    style.PopupRounding = Metrics::PopupRounding;
    style.ScrollbarRounding = Metrics::ScrollbarRounding;
    style.GrabRounding = Metrics::GrabRounding;
    style.TabRounding = Metrics::TabRounding;

    // Borders
    style.WindowBorderSize = Metrics::WindowBorderSize;
    style.ChildBorderSize = Metrics::ChildBorderSize;
    style.PopupBorderSize = Metrics::PopupBorderSize;
    style.FrameBorderSize = Metrics::FrameBorderSize;
    style.TabBorderSize = Metrics::TabBorderSize;
    style.SeparatorTextBorderSize = Metrics::SeparatorTextBorderSize;

    // Alignment
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextPadding = ImVec2(20.0f, 3.0f);

    // Behavior
    style.WindowMenuButtonPosition = ImGuiDir_None;  // Hide window menu button
    style.ColorButtonPosition = ImGuiDir_Right;
    style.TouchExtraPadding = ImVec2(0, 0);

    // Anti-aliasing
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;

    // Misc
    style.CurveTessellationTol = 1.25f;
    style.CircleTessellationMaxError = 0.30f;
    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.50f;
}

void Theme::configureColors(ImGuiStyle& style) {
    ImVec4* colors = style.Colors;

    // Window
    colors[ImGuiCol_WindowBg]             = toVec4(Colors::Abyss);
    colors[ImGuiCol_ChildBg]              = toVec4(Colors::Shadow);
    colors[ImGuiCol_PopupBg]              = toVec4(IM_COL32(16, 16, 24, 245));

    // Borders
    colors[ImGuiCol_Border]               = toVec4(Colors::Slate);
    colors[ImGuiCol_BorderShadow]         = toVec4(Colors::Transparent);

    // Text
    colors[ImGuiCol_Text]                 = toVec4(Colors::Cloud);
    colors[ImGuiCol_TextDisabled]         = toVec4(Colors::Silver);
    colors[ImGuiCol_TextSelectedBg]       = toVec4(Colors::CyanGlow);

    // Frames (inputs, etc.)
    colors[ImGuiCol_FrameBg]              = toVec4(Colors::Smoke);
    colors[ImGuiCol_FrameBgHovered]       = toVec4(Colors::Graphite);
    colors[ImGuiCol_FrameBgActive]        = toVec4(Colors::Slate);

    // Title bar
    colors[ImGuiCol_TitleBg]              = toVec4(Colors::Void);
    colors[ImGuiCol_TitleBgActive]        = toVec4(Colors::Abyss);
    colors[ImGuiCol_TitleBgCollapsed]     = toVec4(Colors::Void);

    // Menu bar
    colors[ImGuiCol_MenuBarBg]            = toVec4(Colors::Abyss);

    // Scrollbar
    colors[ImGuiCol_ScrollbarBg]          = toVec4(Colors::Void);
    colors[ImGuiCol_ScrollbarGrab]        = toVec4(Colors::Slate);
    colors[ImGuiCol_ScrollbarGrabHovered] = toVec4(Colors::Steel);
    colors[ImGuiCol_ScrollbarGrabActive]  = toVec4(Colors::CyanDim);

    // Buttons
    colors[ImGuiCol_Button]               = toVec4(Colors::Smoke);
    colors[ImGuiCol_ButtonHovered]        = toVec4(Colors::CyanDeep);
    colors[ImGuiCol_ButtonActive]         = toVec4(Colors::CyanDark);

    // Headers (collapsing headers, menu items)
    colors[ImGuiCol_Header]               = toVec4(Colors::Obsidian);
    colors[ImGuiCol_HeaderHovered]        = toVec4(Colors::CyanDeep);
    colors[ImGuiCol_HeaderActive]         = toVec4(Colors::CyanDark);

    // Checkboxes, radio buttons
    colors[ImGuiCol_CheckMark]            = toVec4(Colors::Cyan);

    // Sliders
    colors[ImGuiCol_SliderGrab]           = toVec4(Colors::CyanDim);
    colors[ImGuiCol_SliderGrabActive]     = toVec4(Colors::Cyan);

    // Resize grip
    colors[ImGuiCol_ResizeGrip]           = toVec4(Colors::Slate);
    colors[ImGuiCol_ResizeGripHovered]    = toVec4(Colors::CyanDim);
    colors[ImGuiCol_ResizeGripActive]     = toVec4(Colors::Cyan);

    // Tabs
    colors[ImGuiCol_Tab]                  = toVec4(Colors::Obsidian);
    colors[ImGuiCol_TabHovered]           = toVec4(Colors::CyanDeep);
    colors[ImGuiCol_TabActive]            = toVec4(Colors::CyanDark);
    colors[ImGuiCol_TabUnfocused]         = toVec4(Colors::Shadow);
    colors[ImGuiCol_TabUnfocusedActive]   = toVec4(Colors::Obsidian);

    // Docking
    colors[ImGuiCol_DockingPreview]       = toVec4(Colors::CyanGlow);
    colors[ImGuiCol_DockingEmptyBg]       = toVec4(Colors::Void);

    // Plot
    colors[ImGuiCol_PlotLines]            = toVec4(Colors::Cyan);
    colors[ImGuiCol_PlotLinesHovered]     = toVec4(Colors::CyanBright);
    colors[ImGuiCol_PlotHistogram]        = toVec4(Colors::Cyan);
    colors[ImGuiCol_PlotHistogramHovered] = toVec4(Colors::CyanBright);

    // Tables
    colors[ImGuiCol_TableHeaderBg]        = toVec4(Colors::Obsidian);
    colors[ImGuiCol_TableBorderStrong]    = toVec4(Colors::Slate);
    colors[ImGuiCol_TableBorderLight]     = toVec4(Colors::Smoke);
    colors[ImGuiCol_TableRowBg]           = toVec4(Colors::Transparent);
    colors[ImGuiCol_TableRowBgAlt]        = toVec4(IM_COL32(255, 255, 255, 4));

    // Drag and drop
    colors[ImGuiCol_DragDropTarget]       = toVec4(Colors::DropTarget);

    // Navigation
    colors[ImGuiCol_NavHighlight]         = toVec4(Colors::Cyan);
    colors[ImGuiCol_NavWindowingHighlight]= toVec4(IM_COL32(255, 255, 255, 180));
    colors[ImGuiCol_NavWindowingDimBg]    = toVec4(IM_COL32(0, 0, 0, 150));

    // Modal
    colors[ImGuiCol_ModalWindowDimBg]     = toVec4(IM_COL32(0, 0, 0, 180));

    // Separator
    colors[ImGuiCol_Separator]            = toVec4(Colors::Slate);
    colors[ImGuiCol_SeparatorHovered]     = toVec4(Colors::CyanDim);
    colors[ImGuiCol_SeparatorActive]      = toVec4(Colors::Cyan);
}

//=============================================================================
// FONT LOADING
//=============================================================================

bool Theme::loadFonts(float dpiScale) {
    ImGuiIO& io = ImGui::GetIO();

    // Base sizes scaled by DPI
    float sizeSmall   = FontConfig::SizeSmall * dpiScale;
    float sizeRegular = FontConfig::SizeRegular * dpiScale;
    float sizeMedium  = FontConfig::SizeMedium * dpiScale;
    float sizeLarge   = FontConfig::SizeLarge * dpiScale;
    float sizeTitle   = FontConfig::SizeTitle * dpiScale;
    float sizeHero    = FontConfig::SizeHero * dpiScale;

    // The fonts ship in the program's resources folder, wherever the app was started
    // from. ImGui takes UTF-8 paths.
    const std::string pathRegular = utf8FromPath(resourcePath(FontConfig::PathRegular));
    const std::string pathMedium = utf8FromPath(resourcePath(FontConfig::PathMedium));
    const std::string pathBold = utf8FromPath(resourcePath(FontConfig::PathBold));
    const std::string pathMono = utf8FromPath(resourcePath(FontConfig::PathMono));
    auto fileExists = [](const std::string& path) -> bool {
        std::error_code ec;
        return std::filesystem::is_regular_file(pathFromUtf8(path), ec);
    };

    bool hasCustomFonts = fileExists(pathRegular);

    if (!hasCustomFonts) {
        // No custom fonts - use ImGui default with scaled sizes
        ImFontConfig defaultConfig;
        defaultConfig.SizePixels = sizeRegular;
        s_fonts.Regular = io.Fonts->AddFontDefault(&defaultConfig);

        // Create size variants from default
        defaultConfig.SizePixels = sizeSmall;
        s_fonts.Small = io.Fonts->AddFontDefault(&defaultConfig);

        defaultConfig.SizePixels = sizeMedium;
        s_fonts.Medium = io.Fonts->AddFontDefault(&defaultConfig);

        defaultConfig.SizePixels = sizeLarge;
        s_fonts.Large = io.Fonts->AddFontDefault(&defaultConfig);

        defaultConfig.SizePixels = sizeTitle;
        s_fonts.Title = io.Fonts->AddFontDefault(&defaultConfig);

        defaultConfig.SizePixels = sizeHero;
        s_fonts.Hero = io.Fonts->AddFontDefault(&defaultConfig);

        // Mono uses regular for fallback
        s_fonts.Mono = s_fonts.Regular;
        s_fonts.MonoSmall = s_fonts.Small;
        s_fonts.Icons = nullptr;

        // Don't call Build() - modern backends handle this automatically
        return true;
    }

    // Font configuration for custom fonts
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = true;

    // Load custom fonts
    s_fonts.Regular = io.Fonts->AddFontFromFileTTF(pathRegular.c_str(), sizeRegular, &config);
    s_fonts.Small = io.Fonts->AddFontFromFileTTF(pathRegular.c_str(), sizeSmall, &config);

    if (fileExists(pathMedium)) {
        s_fonts.Medium = io.Fonts->AddFontFromFileTTF(pathMedium.c_str(), sizeMedium, &config);
    } else {
        s_fonts.Medium = io.Fonts->AddFontFromFileTTF(pathRegular.c_str(), sizeMedium, &config);
    }

    if (fileExists(pathBold)) {
        s_fonts.Large = io.Fonts->AddFontFromFileTTF(pathBold.c_str(), sizeLarge, &config);
        s_fonts.Title = io.Fonts->AddFontFromFileTTF(pathBold.c_str(), sizeTitle, &config);
        s_fonts.Hero = io.Fonts->AddFontFromFileTTF(pathBold.c_str(), sizeHero, &config);
    } else {
        s_fonts.Large = io.Fonts->AddFontFromFileTTF(pathRegular.c_str(), sizeLarge, &config);
        s_fonts.Title = io.Fonts->AddFontFromFileTTF(pathRegular.c_str(), sizeTitle, &config);
        s_fonts.Hero = io.Fonts->AddFontFromFileTTF(pathRegular.c_str(), sizeHero, &config);
    }

    if (fileExists(pathMono)) {
        s_fonts.Mono = io.Fonts->AddFontFromFileTTF(pathMono.c_str(), sizeRegular, &config);
        s_fonts.MonoSmall = io.Fonts->AddFontFromFileTTF(pathMono.c_str(), sizeSmall, &config);
    } else {
        s_fonts.Mono = s_fonts.Regular;
        s_fonts.MonoSmall = s_fonts.Small;
    }

    s_fonts.Icons = nullptr;  // no icon font is bundled

    // Don't call Build() - modern backends handle this automatically
    return s_fonts.isLoaded();
}

const Theme::Fonts& Theme::fonts() {
    return s_fonts;
}

//=============================================================================
// COLOR UTILITIES
//=============================================================================

ImVec4 Theme::toVec4(ImU32 color) {
    return ImGui::ColorConvertU32ToFloat4(color);
}

ImU32 Theme::toU32(const ImVec4& color) {
    return ImGui::ColorConvertFloat4ToU32(color);
}

ImU32 Theme::withAlpha(ImU32 color, float alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    ImU32 a = static_cast<ImU32>(alpha * 255.0f) << IM_COL32_A_SHIFT;
    return (color & 0x00FFFFFF) | a;
}

ImU32 Theme::lerp(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);

    int ra = (a >> IM_COL32_R_SHIFT) & 0xFF;
    int ga = (a >> IM_COL32_G_SHIFT) & 0xFF;
    int ba = (a >> IM_COL32_B_SHIFT) & 0xFF;
    int aa = (a >> IM_COL32_A_SHIFT) & 0xFF;

    int rb = (b >> IM_COL32_R_SHIFT) & 0xFF;
    int gb = (b >> IM_COL32_G_SHIFT) & 0xFF;
    int bb = (b >> IM_COL32_B_SHIFT) & 0xFF;
    int ab = (b >> IM_COL32_A_SHIFT) & 0xFF;

    int r = static_cast<int>(ra + t * (rb - ra));
    int g = static_cast<int>(ga + t * (gb - ga));
    int bl = static_cast<int>(ba + t * (bb - ba));
    int al = static_cast<int>(aa + t * (ab - aa));

    return IM_COL32(r, g, bl, al);
}

ImU32 Theme::lighten(ImU32 color, float amount) {
    return lerp(color, IM_COL32(255, 255, 255, 255), amount);
}

ImU32 Theme::darken(ImU32 color, float amount) {
    return lerp(color, IM_COL32(0, 0, 0, 255), amount);
}

//=============================================================================
// STYLE PRESETS
//=============================================================================

void Theme::pushAccent() {
    ImGui::PushStyleColor(ImGuiCol_Button, toVec4(Colors::CyanDark));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(Colors::CyanDim));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(Colors::Cyan));
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(Colors::Snow));
}

void Theme::popAccent() {
    ImGui::PopStyleColor(4);
}

void Theme::pushSecondary() {
    ImGui::PushStyleColor(ImGuiCol_Button, toVec4(Colors::MagentaDark));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(Colors::MagentaDim));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(Colors::Magenta));
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(Colors::Snow));
}

void Theme::popSecondary() {
    ImGui::PopStyleColor(4);
}

void Theme::pushDanger() {
    ImGui::PushStyleColor(ImGuiCol_Button, toVec4(Colors::ErrorDim));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(Colors::Error));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(lighten(Colors::Error, 0.2f)));
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(Colors::Snow));
}

void Theme::popDanger() {
    ImGui::PopStyleColor(4);
}

void Theme::pushSuccess() {
    ImGui::PushStyleColor(ImGuiCol_Button, toVec4(Colors::SuccessDim));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(Colors::Success));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(lighten(Colors::Success, 0.2f)));
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(Colors::Snow));
}

void Theme::popSuccess() {
    ImGui::PopStyleColor(4);
}

void Theme::pushWarning() {
    ImGui::PushStyleColor(ImGuiCol_Button, toVec4(Colors::AmberDim));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(Colors::Amber));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(lighten(Colors::Amber, 0.2f)));
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(Colors::Void));
}

void Theme::popWarning() {
    ImGui::PopStyleColor(4);
}

void Theme::pushGhost() {
    ImGui::PushStyleColor(ImGuiCol_Button, toVec4(Colors::Transparent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(Colors::Highlight));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(Colors::Selection));
    ImGui::PushStyleColor(ImGuiCol_Border, toVec4(Colors::Transparent));
}

void Theme::popGhost() {
    ImGui::PopStyleColor(4);
}

void Theme::pushDisabled() {
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    ImGui::BeginDisabled(true);
}

void Theme::popDisabled() {
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
}

void Theme::pushHeader() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec4(Colors::Obsidian));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
}

void Theme::popHeader() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

//=============================================================================
// CUSTOM WIDGETS
//=============================================================================

void Theme::separator(const char* label) {
    if (label && *label) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(Colors::Silver));
        ImGui::SeparatorText(label);
        ImGui::PopStyleColor();
    } else {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Separator, toVec4(Colors::Smoke));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
}

void Theme::sectionHeader(const char* label) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(Colors::Snow));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    // Cyan accent underline
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    float width = ImGui::CalcTextSize(label).x;
    drawList->AddLine(
        ImVec2(pos.x, pos.y - 2),
        ImVec2(pos.x + width, pos.y - 2),
        Colors::Cyan, 2.0f
    );
    ImGui::Spacing();
}

void Theme::subsectionHeader(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(Colors::Silver));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
}

void Theme::tooltipText(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(Colors::Cloud));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void Theme::badge(const char* text, ImU32 bgColor, ImU32 textColor) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 textSize = ImGui::CalcTextSize(text);
    ImVec2 pos = ImGui::GetCursorScreenPos();

    float padding = 6.0f;
    float rounding = 4.0f;
    ImVec2 min = ImVec2(pos.x, pos.y);
    ImVec2 max = ImVec2(pos.x + textSize.x + padding * 2, pos.y + textSize.y + padding);

    drawList->AddRectFilled(min, max, bgColor, rounding);
    drawList->AddText(ImVec2(pos.x + padding, pos.y + padding * 0.5f), textColor, text);

    ImGui::Dummy(ImVec2(textSize.x + padding * 2, textSize.y + padding));
}

void Theme::statusDot(ImU32 color, const char* tooltip) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float radius = 4.0f;
    ImVec2 center = ImVec2(pos.x + radius, pos.y + ImGui::GetTextLineHeight() * 0.5f);

    // Glow
    drawList->AddCircleFilled(center, radius + 2, withAlpha(color, 0.3f));
    // Core
    drawList->AddCircleFilled(center, radius, color);

    ImGui::Dummy(ImVec2(radius * 2 + 4, ImGui::GetTextLineHeight()));

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        tooltipText(tooltip);
        ImGui::EndTooltip();
    }
}

void Theme::statusPulse(ImU32 color, const char* tooltip) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float radius = 4.0f;
    ImVec2 center = ImVec2(pos.x + radius, pos.y + ImGui::GetTextLineHeight() * 0.5f);

    float pulse = pulseAlpha(2.0f, 0.3f, 1.0f);

    // Animated glow
    drawList->AddCircleFilled(center, radius + 3, withAlpha(color, pulse * 0.3f));
    // Core
    drawList->AddCircleFilled(center, radius, withAlpha(color, pulse));

    ImGui::Dummy(ImVec2(radius * 2 + 4, ImGui::GetTextLineHeight()));

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        tooltipText(tooltip);
        ImGui::EndTooltip();
    }
}

void Theme::progressBar(float fraction, const ImVec2& size, ImU32 color, const char* overlay) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz = size;
    if (sz.x < 0) sz.x = ImGui::GetContentRegionAvail().x;
    if (sz.y <= 0) sz.y = 4.0f;

    float rounding = sz.y * 0.5f;
    fraction = std::clamp(fraction, 0.0f, 1.0f);

    // Background
    drawList->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), Colors::Smoke, rounding);

    // Fill
    if (fraction > 0.0f) {
        float fillWidth = sz.x * fraction;
        drawList->AddRectFilled(pos, ImVec2(pos.x + fillWidth, pos.y + sz.y), color, rounding);

        // Glow on the right edge
        if (fraction < 1.0f) {
            drawList->AddCircleFilled(ImVec2(pos.x + fillWidth, pos.y + sz.y * 0.5f),
                                      sz.y * 0.8f, withAlpha(color, 0.3f));
        }
    }

    // Overlay text
    if (overlay && *overlay) {
        ImVec2 textSize = ImGui::CalcTextSize(overlay);
        ImVec2 textPos = ImVec2(pos.x + (sz.x - textSize.x) * 0.5f,
                                pos.y + (sz.y - textSize.y) * 0.5f);
        drawList->AddText(textPos, Colors::Snow, overlay);
    }

    ImGui::Dummy(sz);
}

void Theme::levelMeter(float fraction, const ImVec2& size, ImU32 color) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz = size;
    if (sz.x <= 0) sz.x = 6.0f;
    if (sz.y < 0) sz.y = ImGui::GetContentRegionAvail().y;

    float rounding = sz.x * 0.5f;
    fraction = std::clamp(fraction, 0.0f, 1.0f);

    // Background
    drawList->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), Colors::Smoke, rounding);

    // Fill (from bottom up)
    if (fraction > 0.0f) {
        float fillHeight = sz.y * fraction;
        ImVec2 fillMin = ImVec2(pos.x, pos.y + sz.y - fillHeight);
        ImVec2 fillMax = ImVec2(pos.x + sz.x, pos.y + sz.y);

        // Gradient from green to yellow to red
        ImU32 fillColor = color;
        if (fraction > 0.9f) {
            fillColor = Colors::Error;
        } else if (fraction > 0.7f) {
            fillColor = Colors::Amber;
        }

        drawList->AddRectFilled(fillMin, fillMax, fillColor, rounding);
    }

    ImGui::Dummy(sz);
}

void Theme::panelGradient() {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();

    drawList->AddRectFilledMultiColor(
        pos, ImVec2(pos.x + size.x, pos.y + size.y),
        Colors::GradientTop, Colors::GradientTop,
        Colors::GradientBottom, Colors::GradientBottom
    );
}

void Theme::accentLine(float width) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    if (width < 0) width = ImGui::GetContentRegionAvail().x;

    drawList->AddLine(pos, ImVec2(pos.x + width, pos.y), Colors::Cyan, 2.0f);
    ImGui::Dummy(ImVec2(width, 2.0f));
}

void Theme::shortcutHint(const char* shortcut) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(Colors::Pewter));
    ImGui::TextUnformatted(shortcut);
    ImGui::PopStyleColor();
}

void Theme::icon(const char* iconStr, ImU32 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(color));
    ImGui::TextUnformatted(iconStr);
    ImGui::PopStyleColor();
}

bool Theme::iconButton(const char* iconStr, const char* tooltip, ImU32 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(color));
    ImGui::PushStyleColor(ImGuiCol_Button, toVec4(Colors::Transparent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(Colors::Highlight));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(Colors::Selection));

    float size = ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2;
    bool pressed = ImGui::Button(iconStr, ImVec2(size, size));

    ImGui::PopStyleColor(4);

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        tooltipText(tooltip);
        ImGui::EndTooltip();
    }

    return pressed;
}

//=============================================================================
// LAYOUT HELPERS
//=============================================================================

bool Theme::beginCard(const char* label, bool border) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec4(Colors::Shadow));
    if (!border) {
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    }

    // Use legacy BeginChild API for compatibility
    bool open = ImGui::BeginChild(label, ImVec2(0, 0), border);

    if (!border) {
        ImGui::PopStyleVar();
    }

    return open;
}

void Theme::endCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void Theme::beginButtonGroup() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
}

void Theme::endButtonGroup() {
    ImGui::PopStyleVar(2);
}

void Theme::beginToolbar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec4(Colors::Obsidian));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
}

void Theme::endToolbar() {
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void Theme::beginCentered(float width) {
    float avail = ImGui::GetContentRegionAvail().x;
    float offset = (avail - width) * 0.5f;
    if (offset > 0) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
    }
    ImGui::BeginGroup();
}

void Theme::endCentered() {
    ImGui::EndGroup();
}

//=============================================================================
// ANIMATION HELPERS
//=============================================================================

float Theme::pulseAlpha(float speed, float min, float max) {
    float t = static_cast<float>(ImGui::GetTime()) * speed;
    float s = (std::sin(t) + 1.0f) * 0.5f;  // 0 to 1
    return min + s * (max - min);
}

float Theme::sine(float speed) {
    return std::sin(static_cast<float>(ImGui::GetTime()) * speed);
}

float Theme::time() {
    return static_cast<float>(ImGui::GetTime());
}

} // namespace quadmapper
