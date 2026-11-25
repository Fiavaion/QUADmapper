#pragma once

struct GLFWwindow;

namespace quadmapper {

struct EditorState;
struct AppPreferences;
class Project;
class MediaLibrary;
class SourceManager;
class OutputManager;
class Renderer;
class AudioEngine;
class ThumbnailCache;
class ActionRegistry;
class EditorServices;
namespace control { class ExternalControl; }

// Per-frame bundle of the editor's live state (EditorState) plus the
// Application-owned manager pointers, and the one way a panel reaches them (ADR-051
// §2). Application builds it once per frame and passes it to EditorUi, which hands it
// to every panel. Holds only references/pointers (trivially cheap to construct) and is
// never stored by a panel: it is a transient view. Every pointer may be null (tests,
// the headless harnesses).
struct EditorContext {
    EditorState&   es;
    Project*       project;
    MediaLibrary*  mediaLibrary;
    SourceManager* sourceManager;
    OutputManager* outputManager;
    Renderer*      renderer;
    GLFWwindow*    window;
    AudioEngine*   audio = nullptr;                // sound controls (ADR-050)
    ThumbnailCache* thumbnails = nullptr;          // the media bin's image thumbnails
    control::ExternalControl* control = nullptr;   // OSC / MIDI (ADR-048); null in the harnesses
    AppPreferences* prefs = nullptr;               // the machine preferences
    EditorServices* services = nullptr;            // session, preference saves, AI assist
    ActionRegistry* actions = nullptr;             // editor actions (ADR-051 §3, wired in E6)
};

}  // namespace quadmapper
