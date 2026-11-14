# QUADmapper fonts

The two fonts the interface uses ship in this folder. Both are under the SIL Open Font
License 1.1; the licence text and copyright lines are in `OFL.txt`.

| File | Used for |
|------|----------|
| `HankenGrotesk-Variable.ttf` | All interface text |
| `JetBrainsMono-Variable.ttf` | Numbers, paths and other fixed-width text |

The variable fonts are drawn at their default (Regular) weight. The sizes are set in
`src/ui/Theme.hpp` (`FontConfig`) and scale with the display's DPI setting.

If a font file is missing, QUADmapper falls back to Dear ImGui's built-in font. The
colours and layout stay the same.
