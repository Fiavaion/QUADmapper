# Test Fixtures

This directory contains test fixture files used by unit and integration tests.

## Directory Structure

```
fixtures/
├── projects/          # Sample project files (.qmap)
├── images/            # Test images for media loading
├── videos/            # Test video clips
└── shaders/           # Shader test files
```

## Adding Fixtures

When adding test fixtures:
1. Keep files small (< 1MB for images, < 10MB for videos)
2. Use permissive licenses (CC0, public domain)
3. Document the purpose of each fixture file

## Sample Projects

One hand-written minimal file per shipped `.qmap` format (ADR-032 / ADR-046). Every
one must keep loading forever; `ProjectFixtureTest` (tests/unit/project) loads each
and fails if a `.qmap` is added here without a load check.

- `projects/v1.0-flat.qmap` - flat quad list + group (the v1.0 baseline format)
- `projects/v1.2-flat-grid.qmap` - flat, non-uniform grid, mesh offsets, colour, soft edge
- `projects/v2.0-scenes.qmap` - legacy `scenes` array with `viewState`
- `projects/v3.0-scenes-outputs.qmap` - multi-scene + `outputs` routing + crop
- `projects/v3.1-sources.qmap` - `sources` table + `mediaId` (procedural, missing video)
- `projects/v3.2-objects-projector.qmap` - scene `objects` (missing mesh file) + output `projector` with calibration points
- `projects/v3.3-control-audio-lock.qmap` - MIDI mappings (`control`), master `audio`, a missing clip whose transport is timeline-locked Ping-Pong with volume/mute
- `projects/v3.3-mesh-keystone.qmap` - a keystoned, bezier-edged mesh warp saved before ADR-049 (offsets from the bilinear patch, no `meshBase`), plus a keystoned mesh-edit quad with no offsets
