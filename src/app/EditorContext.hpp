#pragma once

struct GLFWwindow;

namespace quadmapper {

struct EditorState;
class Project;
class MediaLibrary;
class SourceManager;
class OutputManager;
class Renderer;
class AudioEngine;

// Per-frame bundle of the editor's live state (EditorState) plus the
// Application-owned manager pointers. Built once per frame in
// Application::render() and passed to each extracted panel's render(ctx).
// Holds only references/pointers (trivially cheap to construct) and is never
// stored by a panel — it is a transient view. The panels themselves are
// Application's (ui/EditorPanels.hpp, ADR-051). See ADR-0034 / TD-02.
struct EditorContext {
    EditorState&   es;
    Project*       project;
    MediaLibrary*  mediaLibrary;
    SourceManager* sourceManager;
    OutputManager* outputManager;
    Renderer*      renderer;
    GLFWwindow*    window;
    AudioEngine*   audio = nullptr;  // set where a panel shows sound controls (ADR-050)
};

}  // namespace quadmapper
