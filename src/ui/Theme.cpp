#include "Theme.hpp"
#include "core/FileSystem.hpp"
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

} // namespace quadmapper
