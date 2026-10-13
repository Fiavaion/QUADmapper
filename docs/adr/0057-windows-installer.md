# ADR-057: One Windows installer, with the AI runtime bundled

**Date:** 2026-10-13
**Status:** Accepted (owner, 2026-10-13: "A now, B later", Inno Setup)
**Deciders:** Jones

## Context
- QUADmapper has no installer.
- The AI features (ADR-035, ADR-053) need a Python sidecar. Today that means running `tools/ai-sidecar/setup.ps1`: a 5.2 GB venv, 440 MB of models, and Python 3.11 on the machine.
- The owner does not want users to install anything as a separate step.

## Decision
1. **Inno Setup 6** builds one `QUADmapper-Setup-<version>.exe`. The script lives in `packaging/`, and `packaging/build-installer.ps1` stages the files and calls `ISCC`. No CMake change.
2. **Components:**
   - **QUADmapper** (required): the Release build, its DLLs, `resources/`, shaders, and licence notices.
   - **AI features** (optional, ticked by default): the sidecar, a bundled Python runtime and the models.
3. **Bundled Python:**
   - The official python.org **embeddable** Python 3.11 (PSF licence).
   - Packages installed into it from the hash lock (`requirements.lock`, `requirements-sam2.lock`), so the build is reproducible.
   - The models are copied in, with their SHA-256 checked at staging.
   - Installed under `{app}\ai\`. No system Python, no PATH change, no download at install time.
4. **Finding the sidecar:**
   - The app looks for the sidecar next to the executable (`{app}\ai\python\python.exe` and `{app}\ai\sidecar\sidecar.py`) before the dev-tree locations.
   - The sidecar reads its models from `{app}\ai\models`. The existing `QM_MODELS_DIR` override and the dev `.venv` path keep working.
5. **GPU support:** the bundled torch is the CUDA 12.6 build, which falls back to the CPU without an NVIDIA GPU. The AI component is about 2.5–3 GB compressed.
6. **Licences:** `THIRD_PARTY_NOTICES.txt` lists Python, PyTorch, the NVIDIA CUDA runtime (redistributable files only), SAM 2, Depth Anything V2 Small, OpenCV, transformers, and the C++ libraries already linked. The installer shows it.
7. **Later (B, a separate ADR):** move the models to ONNX Runtime (DirectML) inside the app and drop the Python sidecar. That cuts the AI component to about 600 MB and works on any GPU. Users see no change to the installer flow.

## Consequences
- ✅ One download and one install, with no prerequisites. AI can be unticked for a small install.
- ✅ The dev workflow is unchanged, and the bundled runtime is built from the same hash lock.
- ❌ A large installer (about 3 GB with AI).
- ❌ Without a code-signing certificate, Windows SmartScreen warns on first run. Signing is a release task: it needs a certificate the owner buys.
- ❌ Each release re-stages the Python runtime (scripted).

## Alternatives considered
- **PyInstaller-frozen sidecar:** about the same size, harder to debug, and torch freezing is fragile. Rejected.
- **Download the AI parts at install time:** the installer becomes small, but install fails offline or behind firewalls. Rejected: the owner wants nothing separate.
- **WiX/MSI:** better for managed IT deployment, but much more authoring. Inno Setup is enough now, and an MSI can come later.
