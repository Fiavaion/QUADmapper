# QUADmapper Manual Test Protocols

This document provides manual testing procedures for verifying QUADmapper functionality that cannot be fully automated.

## 1. Visual Verification Checklist

### 1.1 Quad Rendering
- [ ] Single quad renders correctly with default corners
- [ ] Corner warping produces smooth perspective correction
- [ ] Mesh subdivision shows smooth interpolation (test resolutions 2, 8, 16, 32, 64)
- [ ] Mesh vertex editing displaces surface correctly
- [ ] UV offset/scale/rotation affects texture mapping
- [ ] Opacity slider fades quad smoothly (0% to 100%)

### 1.2 Texture Display
- [ ] PNG images load and display correctly (with and without alpha)
- [ ] JPG images load and display correctly
- [ ] Large textures (4K+) display without artifacts
- [ ] Texture wrapping respects UV boundaries

### 1.3 Video Playback
- [ ] Video loads and plays at correct frame rate
- [ ] Video maintains sync during playback
- [ ] Speed control (0.1x to 4.0x) affects playback correctly
- [ ] Loop modes work: Once, Loop, Ping-Pong
- [ ] Seek operations are accurate
- [ ] Video texture updates smoothly on quad

### 1.4 Multi-Quad
- [ ] Multiple quads render in correct z-order
- [ ] Selection highlighting is visible
- [ ] Multi-selection (Ctrl+click) works
- [ ] Z-order operations (up/down/top/bottom) work
- [ ] Quad groups affect visibility/lock correctly

### 1.5 User Interface
- [ ] ImGui panels render correctly
- [ ] Docking/undocking panels works
- [ ] All buttons and controls are responsive
- [ ] Text input fields accept keyboard input
- [ ] Sliders and drag controls work smoothly
- [ ] Menu bar and dropdowns function correctly

---

## 2. Performance Benchmark Tests

### 2.1 Setup
1. Launch QUADmapper
2. Open Performance Monitor panel (View > Performance)
3. Ensure GPU is not thermal throttling

### 2.2 Single Quad Performance
| Test | Target | Pass/Fail |
|------|--------|-----------|
| Single quad, no texture | 60 FPS |  |
| Single quad, 1080p texture | 60 FPS |  |
| Single quad, 4K texture | 60 FPS |  |
| Single quad, mesh 64x64 | 60 FPS |  |

### 2.3 Multi-Quad Performance
| Test | Target | Pass/Fail |
|------|--------|-----------|
| 10 quads, 1080p textures | 60 FPS |  |
| 25 quads, 1080p textures | 60 FPS |  |
| 50 quads, 720p textures | 60 FPS |  |
| 100 quads, 720p textures | 45+ FPS |  |

### 2.4 Video Performance
| Test | Target | Pass/Fail |
|------|--------|-----------|
| 1x 1080p30 video | 60 FPS, no drops |  |
| 1x 1080p60 video | 60 FPS, no drops |  |
| 4x 1080p30 videos | 60 FPS, no drops |  |
| 1x 4K30 video | 60 FPS, no drops |  |

### 2.5 Memory Usage
| Test | Max Memory | Pass/Fail |
|------|------------|-----------|
| Idle (no content) | < 200 MB |  |
| 10 quads, 1080p textures | < 500 MB |  |
| 4K video playing | < 1 GB |  |
| 1 hour continuous use | No growth |  |

### 2.6 Startup Time
| Test | Target | Pass/Fail |
|------|--------|-----------|
| Cold start to window visible | < 3 sec |  |
| Load project (10 quads) | < 2 sec |  |
| Load project (50 quads) | < 5 sec |  |

---

## 3. Multi-Output Test Procedure

### 3.1 Prerequisites
- System with 2+ monitors connected
- Monitors configured in Windows Display Settings

### 3.2 Output Window Tests
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Create output | Click "Add Output" | New output appears in list |  |
| Move to monitor | Select monitor from dropdown | Output moves to selected monitor |  |
| Fullscreen toggle | Click fullscreen button | Output fills monitor |  |
| Windowed mode | Press Escape | Output returns to windowed |  |
| Resolution preset | Select from dropdown | Output resizes correctly |  |
| Custom resolution | Enter width/height | Output uses custom size |  |
| Rotation 90 | Set rotation to 90 | Content rotates, resolution swaps |  |
| Rotation 180 | Set rotation to 180 | Content rotates (upside down) |  |
| Rotation 270 | Set rotation to 270 | Content rotates, resolution swaps |  |

### 3.3 Perform Mode Tests
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Enter perform mode | Press F11 or click button | All outputs go fullscreen |  |
| Exit perform mode | Press Escape | All outputs return to windowed |  |
| Borderless fullscreen | Enable borderless option | No window decorations visible |  |
| Multi-monitor perform | With 2+ outputs | All outputs fullscreen simultaneously |  |

### 3.4 Monitor Detection
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Initial enumeration | Launch app | All monitors listed in dropdown |  |
| Hot-plug monitor | Connect new monitor | Monitor appears in list |  |
| Unplug monitor | Disconnect monitor | Monitor removed from list |  |
| Output on unplugged | Remove monitor with output | Output moves to primary |  |

---

## 4. Scene System Tests

### 4.1 Scene Management
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Create scene | Click "New Scene" | New scene added, becomes active |  |
| Rename scene | Double-click, type, Enter | Scene renamed |  |
| Duplicate scene | Click duplicate button | Exact copy created |  |
| Delete scene | Select, click delete | Scene removed (if not last) |  |
| Cannot delete last | Try delete with 1 scene | Delete disabled/blocked |  |
| Reorder scenes | Drag scene in list | Order changes |  |

### 4.2 Scene Switching
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Click to switch | Click scene in list | Scene becomes active |  |
| Keyboard 1-9 | Press number keys | Corresponding scene activates |  |
| Content preserved | Switch and return | All quad positions preserved |  |
| View state preserved | Pan/zoom, switch, return | Pan/zoom preserved |  |

---

## 5. Audio System Tests

### 5.1 Audio Playback
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Video audio plays | Load video with audio | Audio audible |  |
| Master volume | Adjust master slider | Overall volume changes |  |
| Master mute | Toggle master mute | All audio silenced |  |
| Per-source volume | Adjust source slider | Individual source volume changes |  |
| Per-source mute | Toggle source mute | Individual source silenced |  |
| Pan control | Adjust pan L/R | Audio moves between speakers |  |

### 5.2 Device Selection
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Device enumeration | Open audio settings | All devices listed |  |
| Change output device | Select different device | Audio routes to new device |  |
| Default device | Select "Default" | Uses system default |  |

---

## 6. Project Save/Load Tests

### 6.1 Save Tests
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Save new project | Ctrl+S on new project | Save dialog appears |  |
| Save existing | Ctrl+S on saved project | Saves without dialog |  |
| Save As | File > Save As | Save dialog with new name |  |
| Dirty indicator | Modify project | Window title shows * |  |

### 6.2 Load Tests
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Load project | Ctrl+O, select file | Project loads |  |
| v2.0 format | Load v2.0 file | All scenes, settings load |  |
| v1.x format | Load v1.x file | Backward compat works |  |
| Missing textures | Load with missing files | Warning shown, loads anyway |  |
| Corrupted file | Load invalid JSON | Error shown, no crash |  |

---

## 7. Input System Tests

### 7.1 Keyboard Shortcuts
| Test | Shortcut | Expected Result | Pass/Fail |
|------|----------|-----------------|-----------|
| New project | Ctrl+N | New project dialog |  |
| Open project | Ctrl+O | Open dialog |  |
| Save project | Ctrl+S | Save/Save dialog |  |
| Undo | Ctrl+Z | Last action undone |  |
| Redo | Ctrl+Y | Last undo redone |  |
| Select all | Ctrl+A | All quads selected |  |
| Deselect | Escape | Selection cleared |  |
| Delete quad | Delete | Selected quad(s) deleted |  |

### 7.2 Mouse Interaction
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Select quad | Click on quad | Quad selected |  |
| Multi-select | Ctrl+click quads | Multiple quads selected |  |
| Drag corner | Click-drag corner | Corner moves smoothly |  |
| Drag mesh vertex | In mesh mode, drag | Vertex moves smoothly |  |
| Pan view | Middle-drag | View pans |  |
| Zoom view | Scroll wheel | View zooms |  |

---

## 8. Error Handling Tests

### 8.1 Error Recovery
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Invalid texture path | Enter bad path | Error notification shown |  |
| Unsupported video | Load non-video file | Error message, no crash |  |
| GPU memory full | Load many 4K textures | Graceful degradation |  |
| Lost GL context | Force GPU reset | Recovery or clean exit |  |

### 8.2 Error Notifications
| Test | Steps | Expected Result | Pass/Fail |
|------|-------|-----------------|-----------|
| Error appears | Trigger any error | Notification visible |  |
| Auto-dismiss | Wait 5 seconds | Notification fades |  |
| Click dismiss | Click X on notification | Notification closes |  |
| Severity colors | Check different errors | Correct color coding |  |

---

## Test Execution Log

| Date | Tester | Tests Run | Pass | Fail | Notes |
|------|--------|-----------|------|------|-------|
|  |  |  |  |  |  |

---

## Sign-Off

| Role | Name | Date | Signature |
|------|------|------|-----------|
| QA Lead |  |  |  |
| Developer |  |  |  |
| Project Manager |  |  |  |
