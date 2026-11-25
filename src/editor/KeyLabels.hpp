#pragma once

// The names of keys as the user's keyboard prints them, for text shown in the UI.
// String literals, so they join their neighbours: "Blackout (" QM_KEY_CTRL "+B)".
//
// macOS (ADR-062): Dear ImGui reads Cmd as Ctrl there, so every Ctrl chord is typed with
// Cmd; Alt is the Option key; perform mode is Option+P because F11 is a volume key.
#ifdef __APPLE__
#define QM_KEY_CTRL "Cmd"
#define QM_KEY_ALT "Option"
#define QM_KEY_SUPER "Ctrl"  // the physical Control key
#define QM_KEY_PERFORM "Option+P"
#define QM_KEY_QUIT "Cmd+Q"
#else
#define QM_KEY_CTRL "Ctrl"
#define QM_KEY_ALT "Alt"
#define QM_KEY_SUPER "Super"
#define QM_KEY_PERFORM "F11"
#define QM_KEY_QUIT "Alt+F4"
#endif
