#pragma once

struct GLFWwindow;

namespace quadmapper {

struct EditorState;
class Project;
class MediaLibrary;
class SourceManager;
class OutputManager;
class Renderer;

// Per-frame bundle of the editor's live state (EditorState) plus the
// Application-owned manager pointers. Built once per frame in
// Application::render() and passed to each extracted panel's render(ctx).
// Holds only references/pointers (trivially cheap to construct) and is never
// stored by a panel — it is a transient view, so there is no ownership cycle
// even though the panels themselves live inside EditorState. See ADR-0034 / TD-02.
struct EditorContext {
    EditorState&   es;
    Project*       project;
    MediaLibrary*  mediaLibrary;
    SourceManager* sourceManager;
    OutputManager* outputManager;
    Renderer*      renderer;
    GLFWwindow*    window;
};

}  // namespace quadmapper
