#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace quadmapper {

// ---------------------------------------------------------------------------
// StressConfig — CLI-driven perform-mode stress harness (QM-081 H2).
// Fields mirror the --stress sub-flags. `capture` is parsed but not consumed yet.
// ---------------------------------------------------------------------------
struct StressConfig {
    int  cycles         = 50;
    int  outputs        = 1;
    int  framesPerCycle = 120;
    bool fullscreen     = false;
    std::string source  = "procedural";  // "procedural" | "image" | "video" | "hap"
    // H4: path to a media file when --source=video or --source=hap.
    // Required for those source types; empty = error.
    std::string media   = "";
    // H3: hidden mode — exercise the resource lifecycle (per-output Renderer
    // create/destroy) without taking over the screen. Windows stay hidden, and
    // hidden outputs are neither drawn nor swapped (QM-087), so this mode does not
    // exercise the output draw path or vsync pacing.
    bool hidden         = false;
    // H3: working-set bound — FAIL if post-warm-up working set grows more than
    // this many MB across all cycles.
    long workingSetBoundMB = 64;
    // H3: per-cycle wall-clock budget (ms). A cycle that hangs longer than this
    // is flagged as a wedge and aborts the run.
    int  cycleTimeoutMs = 5000;
    // H4: pause mid-cycle then resume to exercise the decode-thread / ring-buffer
    // on resume (MultiMon LESSON-BUG-006 repro). 0 = disabled.
    int  pauseAfter     = 0;
    std::string capture = "";  // "" = disabled
    // ADR-050 §2: with --source=video, replace the video source at the start of every
    // cycle, so its audio attachment (decoder thread, mixer input) is created and freed
    // each cycle under the working-set bound. Audio runs on the silent null device.
    bool audioChurn     = false;
};

// Consume one command-line argument if it is a --stress flag (the first one seen
// creates `cfg`). Returns false for arguments that are not stress flags.
bool parseStressArg(const char* arg, std::optional<StressConfig>& cfg);

// Process working set in bytes (psapi), or 0 when it cannot be read.
std::size_t processWorkingSetBytes();

}  // namespace quadmapper
