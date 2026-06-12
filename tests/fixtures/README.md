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

- `empty.qmap` - Empty project for baseline testing
- `single-quad.qmap` - Project with one quad
- `complex-scene.qmap` - Project with multiple quads, groups, and media
