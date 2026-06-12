# QUADmapper Font Resources

The QUADmapper theme system supports custom fonts for a polished, professional appearance.

## Required Fonts

For the full "Neon Void" visual experience, download and place these fonts in this directory:

### Primary UI Font - Inter
A modern, highly legible sans-serif designed for screen interfaces.

- **Inter-Regular.ttf** - Body text (14px)
- **Inter-Medium.ttf** - Emphasized text (16px)
- **Inter-Bold.ttf** - Headers and titles (20-36px)

Download: https://rsms.me/inter/

### Monospace Font - JetBrains Mono
A developer-focused monospace font for code and numeric values.

- **JetBrainsMono-Regular.ttf** - Code, numeric displays (12-14px)

Download: https://www.jetbrains.com/lp/mono/

### Icon Font - FontAwesome (Optional)
For icon support in the UI.

- **FontAwesome-Solid.ttf** - UI icons (16-18px)

Download: https://fontawesome.com/

## Fallback Behavior

If fonts are not found, the theme will gracefully fall back to ImGui's default font.
The visual appearance will still use the "Neon Void" color palette and styling,
but typography will use the default sans-serif font.

## Installation

1. Download the font files from the links above
2. Copy the .ttf files to this `resources/fonts/` directory
3. Restart QUADmapper

The theme will automatically detect and use the custom fonts.

## Font Sizes (at 1x DPI scale)

| Usage | Font | Size |
|-------|------|------|
| Small labels | Inter Regular | 12px |
| Body text | Inter Regular | 14px |
| Emphasized | Inter Medium | 16px |
| Section headers | Inter Bold | 20px |
| Panel titles | Inter Bold | 26px |
| Hero/splash | Inter Bold | 36px |
| Monospace | JetBrains Mono | 14px |
| Small mono | JetBrains Mono | 12px |
| Icons | FontAwesome | 18px |

Sizes are automatically scaled based on system DPI settings.
