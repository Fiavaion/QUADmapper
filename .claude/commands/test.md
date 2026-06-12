# /test - Run Tests

Run the QUADmapper test suite.

## Usage

```
/test [filter]
```

**Arguments:**
- `filter` (optional): GoogleTest filter pattern (e.g., `QuadTest.*`, `*Serializer*`)

## What This Command Does

1. **Ensure Build is Current**
   ```powershell
   cmake --build build/debug --target all
   ```

2. **Run Tests**
   ```powershell
   ctest --preset debug --output-on-failure
   ```

3. **If Filter Provided**
   ```powershell
   ./build/debug/tests/unit/*_tests.exe --gtest_filter="[filter]"
   ```

4. **Report Results**
   - Show test summary (passed/failed/skipped)
   - Display details for any failures
   - Report test execution time

## Examples

```
/test                    # Run all tests
/test QuadTest.*         # Run all QuadTest cases
/test *Serializer*       # Run tests with "Serializer" in name
/test QuadTest.Create    # Run specific test
```

## Test Executables

| Executable | Tests |
|------------|-------|
| `core_tests` | UUID, Result, FileSystem |
| `scene_tests` | Quad, QuadGeometry, MeshSubdivision, SceneManager |
| `project_tests` | ProjectSerializer, Project |
| `media_tests` | MediaLibrary, ImageLoader |
| `video_tests` | FrameBuffer, TimelineSync |

## Test Output

```
[==========] Running X tests from Y test suites.
[----------] X tests from TestSuite
[ RUN      ] TestSuite.TestName
[       OK ] TestSuite.TestName (Xms)
...
[==========] X tests from Y test suites ran. (Xms total)
[  PASSED  ] X tests.
[  FAILED  ] Y tests.
```
