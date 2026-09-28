#pragma once

#include <imgui.h>
#include <string>

namespace quadmapper {

/**
 * QUADmapper Theme System - "Neon Void"
 *
 * A dark, professional theme inspired by projection mapping and live visual
 * performance tools. Deep blacks with electric cyan and magenta accents
 * that evoke projected light cutting through darkness.
 *
 * Design principles:
 * - Deep, true blacks for OLED-friendly contrast
 * - Cyan primary accent (projection beam aesthetic)
 * - Magenta secondary accent (complementary light)
 * - Generous spacing and refined typography
 * - Subtle glows and gradients for depth
 * - High contrast for dark environment usability
 */
class Theme {
public:
    //=========================================================================
    // COLOR PALETTE - NEON VOID
    //=========================================================================

    struct Colors {
        // === Base Blacks and Grays (Refined — QuadMapper Redesign dc.html) ===
        static constexpr ImU32 Void         = IM_COL32(10, 12, 16, 255);    // #0a0c10 deepest - body/viewport bg
        static constexpr ImU32 Abyss        = IM_COL32(20, 22, 27, 255);    // #14161b panel backgrounds (WindowBg)
        static constexpr ImU32 Shadow       = IM_COL32(22, 25, 31, 255);    // child windows
        static constexpr ImU32 Obsidian     = IM_COL32(27, 30, 37, 255);    // #1b1e25 elevated surfaces (tabs, rows)
        static constexpr ImU32 Smoke        = IM_COL32(31, 35, 43, 255);    // #1f232b frames, inputs, buttons
        static constexpr ImU32 Graphite     = IM_COL32(42, 47, 56, 255);    // #2a2f38 hover backgrounds
        static constexpr ImU32 Slate        = IM_COL32(38, 42, 50, 255);    // #262a32 borders
        static constexpr ImU32 Steel        = IM_COL32(58, 65, 77, 255);    // #3a414d disabled elements
        static constexpr ImU32 Pewter       = IM_COL32(95, 102, 114, 255);  // #5f6672 inactive/dim text
        static constexpr ImU32 Silver       = IM_COL32(154, 161, 173, 255); // #9aa1ad secondary/muted text
        static constexpr ImU32 Cloud        = IM_COL32(224, 227, 233, 255); // body text (ImGuiCol_Text)
        static constexpr ImU32 Mist         = IM_COL32(230, 232, 236, 255); // #e6e8ec primary text
        static constexpr ImU32 Snow         = IM_COL32(244, 246, 250, 255); // bright/header text

        // === Primary Accent - Refined Cyan (#45c4ff, Redesign) ===
        static constexpr ImU32 Cyan         = IM_COL32(69, 196, 255, 255);  // #45c4ff primary accent
        static constexpr ImU32 CyanBright   = IM_COL32(159, 224, 255, 255); // #9fe0ff highlighted/accent text
        static constexpr ImU32 CyanDim      = IM_COL32(47, 143, 214, 255);  // #2f8fd6 hover state
        static constexpr ImU32 CyanDark     = IM_COL32(40, 110, 160, 255);  // pressed/active
        static constexpr ImU32 CyanDeep     = IM_COL32(28, 70, 100, 255);   // subtle accent fill
        static constexpr ImU32 CyanGlow     = IM_COL32(69, 196, 255, 50);   // selection glow
        static constexpr ImU32 CyanFaint    = IM_COL32(69, 196, 255, 20);   // very subtle

        // === Secondary Accent - Hot Magenta (Light Complement) ===
        static constexpr ImU32 Magenta      = IM_COL32(255, 0, 170, 255);   // Secondary accent
        static constexpr ImU32 MagentaBright= IM_COL32(255, 80, 200, 255);  // Highlighted
        static constexpr ImU32 MagentaDim   = IM_COL32(210, 0, 140, 255);   // Hover
        static constexpr ImU32 MagentaDark  = IM_COL32(160, 0, 105, 255);   // Pressed
        static constexpr ImU32 MagentaGlow  = IM_COL32(255, 0, 170, 50);    // Glow

        // === Tertiary - Warm Amber (For warnings, attention) ===
        static constexpr ImU32 Amber        = IM_COL32(255, 180, 0, 255);
        static constexpr ImU32 AmberDim     = IM_COL32(210, 145, 0, 255);
        static constexpr ImU32 AmberGlow    = IM_COL32(255, 180, 0, 50);

        // === Semantic Colors ===
        static constexpr ImU32 Success      = IM_COL32(95, 211, 138, 255);  // #5fd38a (Redesign LIVE green)
        static constexpr ImU32 SuccessDim   = IM_COL32(64, 168, 110, 255);
        static constexpr ImU32 Warning      = IM_COL32(255, 180, 0, 255);   // Amber
        static constexpr ImU32 WarningDim   = IM_COL32(210, 145, 0, 255);
        static constexpr ImU32 Error        = IM_COL32(255, 55, 75, 255);   // Hot red
        static constexpr ImU32 ErrorDim     = IM_COL32(210, 40, 60, 255);
        static constexpr ImU32 ErrorGlow    = IM_COL32(255, 55, 75, 50);
        static constexpr ImU32 Info         = IM_COL32(80, 170, 255, 255);  // Soft blue

        // === Special Purpose ===
        static constexpr ImU32 Selection    = IM_COL32(69, 196, 255, 30);   // List selection bg (rgba .12)
        static constexpr ImU32 Highlight    = IM_COL32(255, 255, 255, 10);  // Subtle hover
        static constexpr ImU32 DropTarget   = IM_COL32(69, 196, 255, 55);   // Drag-drop target
        static constexpr ImU32 Transparent  = IM_COL32(0, 0, 0, 0);

        // === Gradient Pairs (for custom drawing) ===
        static constexpr ImU32 GradientTop    = IM_COL32(18, 21, 27, 255);
        static constexpr ImU32 GradientBottom = IM_COL32(7, 8, 9, 255);
    };

    //=========================================================================
    // METRICS - SPACING AND SIZING
    //=========================================================================

    struct Metrics {
        // Padding
        static constexpr float WindowPadding     = 14.0f;
        static constexpr float FramePaddingX     = 10.0f;
        static constexpr float FramePaddingY     = 6.0f;
        static constexpr float CellPadding       = 6.0f;

        // Spacing
        static constexpr float ItemSpacingX      = 10.0f;
        static constexpr float ItemSpacingY      = 8.0f;
        static constexpr float ItemInnerSpacing  = 6.0f;
        static constexpr float IndentSpacing     = 22.0f;
        static constexpr float ColumnsMinSpacing = 8.0f;

        // Sizing
        static constexpr float ScrollbarSize     = 14.0f;
        static constexpr float GrabMinSize       = 12.0f;
        static constexpr float TabMinWidth       = 80.0f;
        static constexpr float SeparatorHeight   = 1.0f;

        // Rounding - refined corners
        static constexpr float WindowRounding    = 8.0f;
        static constexpr float ChildRounding     = 6.0f;
        static constexpr float FrameRounding     = 5.0f;
        static constexpr float PopupRounding     = 8.0f;
        static constexpr float ScrollbarRounding = 6.0f;
        static constexpr float GrabRounding      = 4.0f;
        static constexpr float TabRounding       = 5.0f;

        // Borders
        static constexpr float WindowBorderSize  = 1.0f;
        static constexpr float ChildBorderSize   = 1.0f;
        static constexpr float PopupBorderSize   = 1.0f;
        static constexpr float FrameBorderSize   = 0.0f;
        static constexpr float TabBorderSize     = 0.0f;
        static constexpr float SeparatorTextBorderSize = 3.0f;
    };

    //=========================================================================
    // FONT CONFIGURATION
    //=========================================================================

    struct FontConfig {
        // Font sizes (will be multiplied by DPI scale)
        static constexpr float SizeSmall    = 12.0f;
        static constexpr float SizeRegular  = 14.0f;
        static constexpr float SizeMedium   = 16.0f;
        static constexpr float SizeLarge    = 20.0f;
        static constexpr float SizeTitle    = 26.0f;
        static constexpr float SizeHero     = 36.0f;

        // Font file paths (relative to resources). Redesign: Hanken Grotesk (UI) +
        // JetBrains Mono (numeric/paths), both OFL — see resources/fonts/README.md.
        // Variable TTFs render at their default (Regular) instance under stb_truetype.
        static constexpr const char* PathRegular = "resources/fonts/HankenGrotesk-Variable.ttf";
        static constexpr const char* PathMedium  = "resources/fonts/HankenGrotesk-Variable.ttf";
        static constexpr const char* PathBold    = "resources/fonts/HankenGrotesk-Variable.ttf";
        static constexpr const char* PathMono    = "resources/fonts/JetBrainsMono-Variable.ttf";
        static constexpr const char* PathIcons   = "resources/fonts/FontAwesome-Solid.ttf";
    };

    // Loaded font references
    struct Fonts {
        ImFont* Small       = nullptr;  // 12px - Captions, labels
        ImFont* Regular     = nullptr;  // 14px - Body text (default)
        ImFont* Medium      = nullptr;  // 16px - Emphasized text
        ImFont* Large       = nullptr;  // 20px - Section headers
        ImFont* Title       = nullptr;  // 26px - Panel titles
        ImFont* Hero        = nullptr;  // 36px - Splash, large display
        ImFont* Mono        = nullptr;  // 14px - Code, numeric values
        ImFont* MonoSmall   = nullptr;  // 12px - Small mono
        ImFont* Icons       = nullptr;  // 18px - Icon font

        bool isLoaded() const { return Regular != nullptr; }
    };

    //=========================================================================
    // PUBLIC API
    //=========================================================================

    // Apply the complete theme (call once during initialization)
    static void apply();

    // Apply to a specific ImGuiStyle (for previews, etc.)
    static void applyToStyle(ImGuiStyle& style);

    // Load fonts with optional DPI scaling
    static bool loadFonts(float dpiScale = 1.0f);

    // Get loaded fonts
    static const Fonts& fonts();

    // Apply dark theme (alias for apply() for compatibility)
    static void applyDark() { apply(); }

    //=========================================================================
    // COLOR UTILITIES
    //=========================================================================

    // Convert ImU32 to ImVec4
    static ImVec4 toVec4(ImU32 color);

    // Convert ImVec4 to ImU32
    static ImU32 toU32(const ImVec4& color);

    // Create color with modified alpha (0.0 - 1.0)
    static ImU32 withAlpha(ImU32 color, float alpha);

    // Interpolate between two colors
    static ImU32 lerp(ImU32 a, ImU32 b, float t);

    // Lighten a color by percentage (0.0 - 1.0)
    static ImU32 lighten(ImU32 color, float amount);

    // Darken a color by percentage
    static ImU32 darken(ImU32 color, float amount);

    //=========================================================================
    // STYLE PRESETS (push/pop patterns)
    //=========================================================================

    // Primary accent style (cyan buttons, etc.)
    static void pushAccent();
    static void popAccent();

    // Secondary accent style (magenta)
    static void pushSecondary();
    static void popSecondary();

    // Danger/destructive action style (red)
    static void pushDanger();
    static void popDanger();

    // Success style (green)
    static void pushSuccess();
    static void popSuccess();

    // Warning style (amber)
    static void pushWarning();
    static void popWarning();

    // Ghost/subtle button style (transparent background)
    static void pushGhost();
    static void popGhost();

    // Disabled visual style
    static void pushDisabled();
    static void popDisabled();

    // Header bar style (for panel headers)
    static void pushHeader();
    static void popHeader();

    //=========================================================================
    // CUSTOM WIDGETS
    //=========================================================================

    // Styled horizontal separator with optional label
    static void separator(const char* label = nullptr);

    // Section header with accent underline
    static void sectionHeader(const char* label);

    // Subsection header (smaller, no underline)
    static void subsectionHeader(const char* label);

    // Styled tooltip (call between BeginTooltip/EndTooltip)
    static void tooltipText(const char* text);

    // Badge/tag with color
    static void badge(const char* text, ImU32 bgColor = Colors::CyanDark,
                     ImU32 textColor = Colors::Snow);

    // Status indicator dot
    static void statusDot(ImU32 color, const char* tooltip = nullptr);

    // Animated status indicator (pulsing)
    static void statusPulse(ImU32 color, const char* tooltip = nullptr);

    // Progress bar with glow effect
    static void progressBar(float fraction, const ImVec2& size = ImVec2(-1, 4),
                           ImU32 color = Colors::Cyan, const char* overlay = nullptr);

    // Vertical progress/level meter
    static void levelMeter(float fraction, const ImVec2& size = ImVec2(6, -1),
                          ImU32 color = Colors::Cyan);

    // Gradient panel background (call at start of window)
    static void panelGradient();

    // Accent line decoration
    static void accentLine(float width = -1);

    // Keyboard shortcut display
    static void shortcutHint(const char* shortcut);

    // Icon with optional color
    static void icon(const char* icon, ImU32 color = Colors::Cloud);

    // Icon button (square, icon only)
    static bool iconButton(const char* icon, const char* tooltip = nullptr,
                          ImU32 color = Colors::Cloud);

    //=========================================================================
    // LAYOUT HELPERS
    //=========================================================================

    // Begin a styled card/panel
    static bool beginCard(const char* label, bool border = true);
    static void endCard();

    // Horizontal button group (no gaps between buttons)
    static void beginButtonGroup();
    static void endButtonGroup();

    // Toolbar area
    static void beginToolbar();
    static void endToolbar();

    // Centered content
    static void beginCentered(float width);
    static void endCentered();

    //=========================================================================
    // ANIMATION HELPERS
    //=========================================================================

    // Get pulsing alpha value (for blinking indicators)
    static float pulseAlpha(float speed = 2.0f, float min = 0.4f, float max = 1.0f);

    // Get smooth sine wave value
    static float sine(float speed = 1.0f);

    // Get current animation time
    static float time();

private:
    static Fonts s_fonts;

    static void configureStyle(ImGuiStyle& style);
    static void configureColors(ImGuiStyle& style);
    static void configureColorsDark(ImGuiStyle& style);
};

//=============================================================================
// ICON CONSTANTS (FontAwesome subset)
//=============================================================================

namespace Icons {
    // Media controls
    constexpr const char* Play          = "\xEF\x81\x8B";  //
    constexpr const char* Pause         = "\xEF\x81\x8C";  //
    constexpr const char* Stop          = "\xEF\x81\x8D";  //
    constexpr const char* Forward       = "\xEF\x81\x8E";  //
    constexpr const char* Backward      = "\xEF\x81\x8A";  //
    constexpr const char* StepForward   = "\xEF\x81\x91";  //
    constexpr const char* StepBackward  = "\xEF\x81\x88";  //

    // File operations
    constexpr const char* FolderOpen    = "\xEF\x81\xBC";  //
    constexpr const char* File          = "\xEF\x85\x9B";  //
    constexpr const char* Save          = "\xEF\x83\x87";  //
    constexpr const char* Download      = "\xEF\x80\x99";  //
    constexpr const char* Upload        = "\xEF\x82\x93";  //

    // Editing
    constexpr const char* Plus          = "\xEF\x81\xA7";  //
    constexpr const char* Minus         = "\xEF\x81\xA8";  //
    constexpr const char* Times         = "\xEF\x80\x8D";  //
    constexpr const char* Check         = "\xEF\x80\x8C";  //
    constexpr const char* Edit          = "\xEF\x8C\x84";  //
    constexpr const char* Trash         = "\xEF\x87\xB8";  //
    constexpr const char* Copy          = "\xEF\x83\x85";  //
    constexpr const char* Undo          = "\xEF\x83\xA2";  //
    constexpr const char* Redo          = "\xEF\x80\x9E";  //

    // View
    constexpr const char* Eye           = "\xEF\x81\xAE";  //
    constexpr const char* EyeSlash      = "\xEF\x81\xB0";  //
    constexpr const char* Expand        = "\xEF\x81\xA5";  //
    constexpr const char* Compress      = "\xEF\x81\xA6";  //
    constexpr const char* Search        = "\xEF\x80\x82";  //
    constexpr const char* ZoomIn        = "\xEF\x80\x8E";  //
    constexpr const char* ZoomOut       = "\xEF\x80\x90";  //

    // Arrows
    constexpr const char* ArrowUp       = "\xEF\x81\xA2";  //
    constexpr const char* ArrowDown     = "\xEF\x81\xA3";  //
    constexpr const char* ArrowLeft     = "\xEF\x81\xA0";  //
    constexpr const char* ArrowRight    = "\xEF\x81\xA1";  //
    constexpr const char* ChevronUp     = "\xEF\x81\xB7";  //
    constexpr const char* ChevronDown   = "\xEF\x81\xB8";  //
    constexpr const char* ChevronLeft   = "\xEF\x81\x93";  //
    constexpr const char* ChevronRight  = "\xEF\x81\x94";  //

    // Objects
    constexpr const char* Cog           = "\xEF\x80\x93";  //
    constexpr const char* Lock          = "\xEF\x80\xA3";  //
    constexpr const char* Unlock        = "\xEF\x82\x9C";  //
    constexpr const char* Image         = "\xEF\x80\xBE";  //
    constexpr const char* Video         = "\xEF\x80\x88";  //
    constexpr const char* Music         = "\xEF\x80\x81";  //
    constexpr const char* VolumeUp      = "\xEF\x80\xA8";  //
    constexpr const char* VolumeMute    = "\xEF\x9A\xA9";  //
    constexpr const char* Display       = "\xEF\x84\x88";  //
    constexpr const char* Grid          = "\xEF\x80\x8A";  //
    constexpr const char* Layers        = "\xEF\x97\xBD";  //
    constexpr const char* Cube          = "\xEF\x86\xB2";  //

    // Status
    constexpr const char* CircleDot     = "\xEF\x86\x92";  //
    constexpr const char* Circle        = "\xEF\x84\x91";  //
    constexpr const char* Warning       = "\xEF\x81\xB1";  //
    constexpr const char* Info          = "\xEF\x81\x9A";  //
    constexpr const char* Question      = "\xEF\x81\x99";  //
}

} // namespace quadmapper
