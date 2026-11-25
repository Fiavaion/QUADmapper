# QUADmapper 1.0.0: website launch kit

Everything the QUADmapper website needs for the 1.0.0 launch, taken from the repo on 2026-11-23 (`dev` at 590bedb). The sections marked **copy** can go on the site as written; the rest are facts and checklists for building it.

Sources: [RELEASE_NOTES.md](../RELEASE_NOTES.md), [README.md](../README.md), [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md), [quick-start.html](../resources/docs/quick-start.html), [ai-features-guide.html](../resources/docs/ai-features-guide.html), [notes/CURRENT_STATUS.md](../notes/CURRENT_STATUS.md).

---

## 1. Fix these before launch

These affect what the site can promise or link to.

| # | Item | Why it matters for the site | Where |
|---|---|---|---|
| 1 | **Done 2026-11-23: the GitHub repo is public.** | Help > Report a Bug (`https://github.com/Fiavaion/Quadmapper/issues/new`) and the FFmpeg written offer now reach the issues page. The download is the release asset: `https://github.com/Fiavaion/QUADmapper/releases/download/v1.0.0/QUADmapper-Setup-1.0.0.exe`, live once the v1.0.0 draft release is published. | GitHub |
| 2 | **The installer has no AI engine.** | `packaging/build-installer.ps1` makes `QUADmapper-Setup-1.0.0.exe` (10.5 MB, Inno Setup): Program Files or per-user, Start-menu shortcuts, an uninstaller. It is the file to put on the download page. It was tested by a silent install, a stress run and a video export from the installed copy, and an uninstall. | TD-44 |
| 3 | **The AI features are not in the release folder.** | The AI engine (Python, models) is not packaged yet, so the download has no AI. Either leave AI off the launch page or label it "coming soon". | TD-44 |
| 4 | **The exe is not code-signed yet.** | Windows SmartScreen shows "Windows protected your PC" on first run. SignPath Foundation declined free signing on 2026-11-24 because the project doesn't yet have enough visibility; apply again once it has some following. `.github/workflows/release.yml` already builds and signs on GitHub if SignPath is set up, and builds unsigned until then. If they accept, restore a code signing policy page (deleted in the commit after `7b674c6`): the download page must say "Code signing policy" and link to it. Until then, keep the "More info > Run anyway" note on the download page (section 8 has the text). | Unsigned; reapply to SignPath later |
| 5 | **No release date yet.** | The release notes say "the date is set when the release is made". | RELEASE_NOTES.md |
| 6 | **The new features have not been tried in the GUI yet.** | Video export, compositions, projector auto-detect, grid warp and the other-scenes outlines all pass their automated tests, but no one has clicked through them yet. Do that before you advertise them or take screenshots of them. | owner test list |
| 7 | **Open source or not?** | The licence is MIT. If the site says "open source", the code needs to be public (see item 1). | LICENSE |

---

## 2. The basics

| Field | Value |
|---|---|
| Product name | QUADmapper |
| Version | 1.0.0 (first public release) |
| Publisher / copyright | Fiavaion, © 2026 |
| Licence | MIT (free to use, including commercially) |
| Platform | Windows 10 and 11, 64-bit |
| Category | Projection mapping / video mapping software |
| Made for | Live-event technicians, VJs, installation artists, architectural projection designers, theatre and stage designers |
| Project file | `.qmap` (format 3.7.0) |
| Download form | A zip of the app folder today; an installer later |

---

## 3. Headlines and descriptions (copy)

**Tagline options**
- Line up a projector in minutes. Keep the show running.
- Projection mapping for Windows, made for the show.
- Warp, map and play on any surface.

**One sentence (meta description, about 155 characters)**
> QUADmapper is free projection mapping software for Windows. Warp video onto any surface, run several projectors and control the show by OSC and MIDI.

**Short paragraph**
> QUADmapper is a projection mapping application for Windows. It warps images, video and generated patterns onto real surfaces, so a projector's picture lines up with a wall, a screen or a stage set. It is made for technicians, VJs and designers who need to line up a projector quickly and keep the show running.

**Longer paragraph**
> Pull a quad's four corners onto your surface, or turn on its grid warp for curved walls and set pieces. Load video, HAP, images or built-in test patterns, build scenes, and take them on air with a key press or a crossfade. Run several projectors, with slices and edge blending where they overlap. During the show, perform mode, blackout and a perform lock keep things safe, and a lighting desk or MIDI controller can drive it all. When the show has to run without a PC, export each projector's picture as an MP4 for a media player.

---

## 4. Features (copy)

The headings work as website sections. Each bullet is one short line.

### Map any surface
- **Corner pin:** drag the four corners, or pick one with `Tab` and nudge it with the arrow keys (`Shift` 10x, `Alt` 0.1x).
- **Linear or Perspective warp** for each quad. Linear lines up quads snapped edge to edge; Perspective keeps lines straight on a tilted surface.
- **Grid warp** for curved surfaces: add rows and columns where you need them, drag single points or whole lines, with Spline (smooth) or Linear points.
- **Warp or Crop:** bend the picture with the grid, or keep it flat and cut a shape out of it.
- **Per-quad crop, colour correction and soft edges.** Group, lock and hide quads.
- **Snap and outlines:** see the other scenes' quads as outlines and snap to them, for two projectors stacked on one surface.
- **3D objects:** import an OBJ model and calibrate a projector against it by clicking matching points.
- **Test grid** on every new quad, so lining up starts at once.

### Play anything
- **Video** with sound, decoded on the graphics card where it can be, so more and larger videos play smoothly. One video can feed many quads.
- **HAP and HAP Q** playback, plus **Convert to HAP** in the media bin.
- **Images:** PNG, JPG, TIF/TIFF, BMP, TGA.
- **Built-in patterns:** solid colour, gradient, checkerboard, grid, SMPTE bars, noise, plasma.
- **Webcam** as a source.
- **Seamless loops,** Ping-Pong, speed control and a master timeline that starts several videos together.

### Compositions *(new)*
- A **Timeline** plays clips, images and patterns on one quad, one after another, with a crossfade between them.
- A **Switcher** changes clip only when told: Next, Previous or Cue n.
- Driven from the app, by **OSC** (`/qm/composition/<name>/next`, `/prev`, `/cue`, `/play`, `/pause`, `/restart`, `/fade`) or by **MIDI learn**.
- The sound crossfades with the picture.

### Scenes and shows
- Several **scenes** in one project. Put one on air with keys `1` to `9`, as a cut or a crossfade.
- **Projectors are found automatically:** each screen gets an output at its own resolution and a scene of its own.
- **Slices and edge blending** (width, gamma, curve, auto-blend) join overlapping projectors.
- **Perform mode** (`F11`), **blackout** (`Ctrl+B`) and a **perform lock** (`Ctrl+Shift+L`) that blocks edits mid-show while the show controls keep working.

### Export for media players *(new)*
- **File > Export Video...** renders each projector's output to its own **H.264 MP4 with sound**, at that output's resolution and rotation.
- Play the files from a media player, with no PC at the venue.
- The length can be the video's own (when one video is mapped) or a time you set. 25, 30, 50 or 60 fps.
- Every frame is rendered, however busy the PC is.

### External control
- **OSC** input (port 8000, off until turned on), address patterns with wildcards, and feedback to a controller.
- **MIDI** input with **MIDI learn**, 14-bit controllers, and mappings saved in the project.

### Built for the show
- **Safe saves and autosave** (every minute by default) with **crash recovery**.
- Asks before deleting scenes, outputs or media that quads use.
- A remembered panel layout, **Simple and Advanced** modes, and keys you can reassign.
- Drag a `.qmap` onto the window to open it. Open Recent lists the last ten shows.

### AI assist (beta, not in the 1.0.0 download yet; see section 1, item 3)
- **Scan Output:** the projector shows stripe patterns and a webcam maps itself to the projector.
- **Pick Surface / Detect Surface:** click a surface in a photo, or let the app propose one, and get the corners.
- **One-Flash:** one pattern and one photo give a flat-surface placement and an anamorphic warp.
- **Place From Photo** for 3D calibration.
- Off until turned on in Preferences. Mapping never needs it.

---

## 5. System requirements (copy)

**Minimum**
- Windows 10 or 11, 64-bit
- A graphics card with OpenGL 3.3 or later (OpenGL 4.3 for the built-in patterns)
- 4 GB RAM
- 100 MB of disk space

**Recommended**
- Windows 11, 64-bit
- An NVIDIA GeForce GTX 1060 or equivalent
- 8 GB RAM
- An SSD for video content

**For the AI features (beta, when they ship):** a webcam; an NVIDIA graphics card for speed (without one they run on the CPU and are much slower); about 5.5 GB of disk space for the AI engine and its models.

> The 4 GB / 100 MB / GTX 1060 figures come from the December 2025 milestone notes. Check the size of the 1.0.0 app folder after `cmake --install` and correct the disk figure.

Performance measured on the dev PC (RTX 3080 Laptop, Release build), usable as a "how fast" line:
- 16 HAP streams at 1080p across two outputs: 159 to 164 fps, every frame presented.
- 256 quads on one 1080p output: about 60 fps.
- Key press to picture: 19 ms (p95, uncapped).

---

## 6. Getting started (copy, for a "first mapping" section)

1. Download and unzip QUADmapper, then run `QUADmapper.exe`. A quad with the test grid is already on the canvas.
2. Open the **Media** tab, click **Load Files...**, and drag a thumbnail onto the quad.
3. Click a corner (or press `Tab`) and nudge it with the arrow keys until the picture sits on your surface. Repeat for each corner.
4. Press **F11** to send it to the projector. `Esc` comes back.
5. Save with `Ctrl+S`.

The full guide ships with the app (Help > User Guide); it is `resources/docs/quick-start.html`, a self-contained page that could also go on the site as-is.

---

## 7. Keyboard shortcuts (copy)

| Keys | What it does |
|---|---|
| `Tab` / `Shift+Tab` | Select the next or previous corner or mesh point |
| Arrow keys | Nudge the selected corner (`Shift` 10x, `Alt` 0.1x) |
| `H` / `Shift+H` | Cycle the grid-line handles of the selected point |
| `Ctrl+M` | Toggle mesh edit |
| `Alt+=` / `Alt+-` | Finer or coarser mesh |
| `Alt+R` | Reset mesh |
| `Ctrl+Z` / `Ctrl+Y` or `Ctrl+Shift+Z` | Undo / Redo |
| `Ctrl+A` / `Delete` | Select all / Delete selected |
| `Ctrl+G` | Group selected |
| `Ctrl+]` / `Ctrl+[` | Bring forward / Send backward |
| `Ctrl+Shift+]` / `Ctrl+Shift+[` | Bring to front / Send to back |
| `Space` (tap) | Play or pause every video |
| `Space` (hold) + drag | Pan the canvas |
| `1` to `9` | Take scene 1 to 9 |
| `F11` | Perform mode on or off |
| `Esc` | Leave perform mode |
| `Ctrl+B` | Blackout |
| `Ctrl+Shift+L` | Perform lock |
| `Ctrl+N` / `Ctrl+O` | New / Open project |
| `Ctrl+S` / `Ctrl+Shift+S` | Save / Save as |
| `Ctrl+,` | Preferences |

Most keys can be changed in Edit > Preferences > Keyboard. `Tab`, the arrows and `H` cannot be reassigned; `F11`, `Esc` and `Ctrl+B` are fixed.

---

## 8. Download page (copy and facts)

**What is in the download:** `QUADmapper.exe`, the FFmpeg and other DLLs, the Visual C++ runtime (nothing else to install), the user guide, `LICENSE.txt` and `THIRD-PARTY-NOTICES.txt`. Settings, layout and logs are kept in the user's own folders, so the app folder can live anywhere.

**First-run note (while the exe is unsigned):**
> Windows may show "Windows protected your PC" the first time you run QUADmapper, because the program is new and not yet signed. Click **More info**, then **Run anyway**.

**Put beside the download link:**
- version 1.0.0, release date, file size;
- a SHA-256 checksum of the zip (`Get-FileHash QUADmapper-1.0.0.zip -Algorithm SHA256`);
- links to the release notes, the user guide, the licence and the third-party notices.

**Safety note** (from the release review; it is also in the guide):
> Open projects only from people you trust. A project can point at media on a network share, and opening it makes Windows connect to that computer.

---

## 9. Licences and legal (needed on the site)

- **QUADmapper:** MIT, © 2026 Fiavaion. Link `LICENSE`.
- **FFmpeg** (LGPL 2.1 or later), linked as DLLs beside the exe; built without GPL or non-free parts. The LGPL **requires**:
  - the notices shipped with the download (done: `THIRD-PARTY-NOTICES.txt`);
  - the FFmpeg source to be available. The notices name https://ffmpeg.org/releases/ (`ffmpeg-9.0.2.tar.xz`) and a written offer for three years. **The offer currently says to ask through GitHub issues, which the public cannot reach (section 1, item 1).** A link on the download page to the FFmpeg 9.0.2 source, plus a contact address for the offer, covers it.
- Other components (all permissive): GLFW, GLM, nlohmann/json, miniaudio, Dear ImGui, GLAD, stb_image and stb_dxt, the Snappy notice, the HAP specification. Fonts: Hanken Grotesk and JetBrains Mono (SIL Open Font License).
- **AI features (when shipped):** SAM 2.1 small and tiny, and Depth Anything V2 Small, all Apache-2.0, plus the Python packages listed in the notices.
- A "Licences" or "Open-source credits" page can simply host `THIRD-PARTY-NOTICES.md`.

---

## 10. Release notes for the site

The complete, user-facing list is the **1.0.0** section of [RELEASE_NOTES.md](../RELEASE_NOTES.md). Use it as the changelog page. Leave out everything below the line "Development milestone, December 2025 (not published)".

**Known limitations** (copy; worth a section on the release notes page):
- With displays at different refresh rates, the show runs at the rate of the slowest one.
- No MIDI Timecode or MIDI clock sync. OSC bundle time tags are ignored.
- OSC feedback and 14-bit MIDI have not been tried with real controllers.
- Audio delay is estimated, not measured; the projector's own delay is not compensated.
- Under heavy load, opening or removing a video can cause a pause of up to about 0.05 s.
- A composition's sound is not in exported video files yet (the picture is).
- AI beta: Detect Surface was tuned on a few test images and may propose the wrong surface on some venues; Pick Surface is the fallback.

---

## 11. FAQ (copy)

**Is it free?** Yes. QUADmapper is released under the MIT licence: free to use for any show, commercial ones included.

**Does it run on Mac or Linux?** Not yet. QUADmapper 1.0 is for Windows 10 and 11.

**How many projectors can I use?** As many as your PC has outputs. QUADmapper finds each one at startup and gives it its own output and scene. Overlapping projectors can be edge-blended.

**Can I play the show without a PC?** Yes. File > Export Video renders each projector's picture to an MP4 file with sound, for a media player.

**What video formats does it play?** Anything FFmpeg decodes: H.264, HEVC, ProRes and more, plus HAP and HAP Q for heavy shows. It can convert other video to HAP for you.

**Can a lighting desk control it?** Yes, by OSC (UDP, port 8000 by default) or MIDI with MIDI learn: scenes, blackout, compositions and more.

**What happens if it crashes mid-show?** Autosave writes a recovery copy every minute by default, and QUADmapper offers to restore it on the next start.

**Do I need the AI features?** No. Mapping never needs them. They are an optional beta for camera-assisted setup.

---

## 12. Assets to make for the site

None of these exist in the repo yet.

- [ ] **App icon** for the site and favicon: the exe's icon is `src/app/QUADmapper.ico`.
- [ ] **Hero screenshot:** the editor with a mapped show (Advanced mode, dark theme).
- [ ] **Screenshots**, one per feature section: grid warp on a curved surface; Outputs tab with slices and edge blend; Compositions tab; Export Video dialog; Control (OSC/MIDI) window.
- [ ] **Photo or video of a real projection** (before and after alignment) is the strongest image for this kind of product.
- [ ] **How-to videos:** the owner's recordings on the work PC (CURRENT_STATUS, "Now").
- [ ] **Brand fonts** if the site should match the app: Hanken Grotesk and JetBrains Mono, both in `resources/fonts/` (OFL).
- [ ] **Social preview image** (1200x630) and the meta description from section 3.

---

## 13. Pages the site needs

1. **Home:** tagline, short paragraph, hero image, three or four feature highlights, the download button.
2. **Features:** section 4.
3. **Download:** section 8, plus the system requirements (section 5).
4. **Getting started / docs:** section 6 and the quick-start guide.
5. **Release notes:** section 10.
6. **Licences:** section 9.
7. **Support / contact:** the bug report and FFmpeg-source address (section 1, item 1).
8. **FAQ:** section 11.
