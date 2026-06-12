# /build - Build QUADmapper

Build the QUADmapper project using CMake.

## Usage

```
/build [preset]
```

**Arguments:**
- `preset` (optional): Build preset - `debug` (default), `release`, or `relwithdebinfo`

## What This Command Does

1. **Check Prerequisites**
   - Verify VCPKG_ROOT environment variable is set
   - Check CMake is available

2. **Configure (if needed)**
   ```powershell
   cmake --preset [preset]
   ```

3. **Build**
   ```powershell
   cmake --build build/[preset]
   ```

4. **Report Results**
   - Show build success/failure
   - List any errors with file locations
   - Report build time

## Examples

```
/build          # Build debug (default)
/build debug    # Build debug explicitly
/build release  # Build release
```

## Build Presets

| Preset | Description |
|--------|-------------|
| `debug` | Debug build with tests, console output |
| `release` | Optimized release build |
| `relwithdebinfo` | Release with debug symbols for profiling |

## Output Location

- Debug: `build/debug/QUADmapper.exe`
- Release: `build/release/QUADmapper.exe`
- RelWithDebInfo: `build/relwithdebinfo/QUADmapper.exe`
