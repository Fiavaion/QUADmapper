#pragma once

#include <optional>
#include <string>

namespace quadmapper {

// ---------------------------------------------------------------------------
// BenchConfig — the headless PRD §5 performance benchmark (--bench). Every flag is
// --bench-prefixed so it never collides with the --stress sub-flags.
//
//   --bench[=LIST]            scenarios, comma-separated: quads256, videos16,
//                             sceneSwitch, latency, drift, sameFrame (default: all);
//                             present (TD-12: fullscreen outputs on every monitor,
//                             per-output swap timing) runs only when named
//   --bench-out=PATH          JSON report file (also printed to stdout); the log
//                             goes to PATH.log
//   --bench-seconds=S         measured seconds per scenario (after warm-up)
//   --bench-warmup=S          warm-up seconds per scenario (not measured)
//   --bench-outputs=N         1080p projector outputs (windowed, on screen)
//   --bench-monitor=N         monitor the output windows open on (its refresh
//                             rate paces --bench-vsync=1)
//   --bench-vsync=0|1         0 = uncapped (measures headroom), 1 = the app's
//                             own vsync pacing
//   --bench-quads=N           quads per scene in quads256
//   --bench-videos=N          simultaneous video sources in videos16
//   --bench-media=PATH        video file for videos16 / drift
//   --bench-hap-media=PATH    HAP file for the videos16 HAP case ("" = skip)
//   --bench-scenes=N          scenes cycled by sceneSwitch (2-9: taken by keys 1-9)
//   --bench-switch-every=K    frames between scene switches
//   --bench-latency-samples=N nudges measured by latency
//   --bench-drift-seconds=S   playback length of the drift scenario
// ---------------------------------------------------------------------------
struct BenchConfig {
    std::string scenarios = "all";
    std::string outPath = "bench.json";
    double seconds = 10.0;
    double warmupSeconds = 2.0;
    int outputs = 2;
    int monitor = 0;
    bool vsync = false;
    int quads = 256;
    int videos = 16;
    std::string media = "D:/testing Videos/greenCrystals_15Sec.mp4";
    std::string hapMedia = "D:/testing Videos/Hap/5sec_hap.mov";
    int scenes = 4;
    int switchEvery = 30;
    int latencySamples = 60;
    double driftSeconds = 30.0;

    std::string logPath() const { return outPath + ".log"; }
    bool wants(const std::string& scenario) const;
};

// Consume one command-line argument if it is a --bench flag (the first one seen
// creates `cfg`). Returns false for arguments that are not bench flags.
bool parseBenchArg(const char* arg, std::optional<BenchConfig>& cfg);

}  // namespace quadmapper
