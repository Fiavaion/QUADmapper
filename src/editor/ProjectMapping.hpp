#pragma once

// ---- Editor <-> Project persistence bridge (ADR-039 P5) ----
// The live state of truth is EditorState's scene list (Scene/QuadManager) plus the
// OutputManager's output config; the Project DTO is only the serialization shape.
// These two helpers are the single place that copies between them. Save gathers EVERY
// scene + output into the DTO (the serializer then writes .qmap v3); load rebuilds the
// live scenes/outputs and remaps each scene's output assignment onto the freshly
// created output ids. Quad geometry + texture binding (incl. mesh offsets, flipH/V,
// colour, soft edge) copy whole, so nothing is dropped on round-trip. Each bound
// SourceManager source is described into the v3.1 "sources" table (ADR-046).

#include <vector>

namespace quadmapper {

struct EditorState;
class OutputManager;
class OutputWindow;
class SourceManager;
class MediaLibrary;
class Project;

void syncEditorToProject(EditorState& es, OutputManager* om, SourceManager* sm, Project& project);

// Media and mesh files that cannot be opened never fail the load (PRD §4.1.8): their
// quads and objects keep the reference (EditorState::missingSources; an object keeps
// its mesh path with an empty mesh) and collectMissingMedia() lists them to relocate.
// The sources of the media bin's `library` survive the old project's release.
void syncProjectToEditor(EditorState& es, OutputManager* om, const Project& project,
                         SourceManager* sm, const MediaLibrary* library);

// ---- One scene per projector (owner, 2026-11-23) ----
// Each of `outputs` that shows no scene gets the first scene no output shows, else a
// new scene ("Scene N") with one quad, selected. routeScene sizes each canvas to its
// output. The active scene (created first if there is none) stays the active one.
// Returns how many outputs got a scene.
int giveOutputsTheirOwnScenes(EditorState& es, OutputManager& om,
                              const std::vector<OutputWindow*>& outputs);

}  // namespace quadmapper
