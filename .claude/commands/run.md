# /run - Run QUADmapper

Run the QUADmapper application.

## Usage

```
/run [preset]
```

**Arguments:**
- `preset` (optional): Build preset to run - `debug` (default) or `release`

## What This Command Does

1. **Ensure Build is Current**
   ```powershell
   cmake --build build/[preset] --target QUADmapper
   ```

2. **Run Application**
   ```powershell
   ./build/[preset]/QUADmapper.exe
   ```

## Examples

```
/run          # Run debug build (default)
/run debug    # Run debug build explicitly
/run release  # Run release build
```

## Debug vs Release

| Aspect | Debug | Release |
|--------|-------|---------|
| Console | Yes (visible) | No (hidden) |
| Performance | Slower | Optimized |
| Debug symbols | Yes | No |
| Assertions | Enabled | Disabled |

## Working Directory

The application runs from the project root directory to ensure resources are found correctly.

## Common Issues

**Missing DLLs:** Ensure vcpkg DLLs are in PATH or copied to build directory.

**OpenGL errors:** Check GPU driver is up to date and supports OpenGL 3.3+.

**Resource not found:** Verify resources/ directory is copied to build output.
