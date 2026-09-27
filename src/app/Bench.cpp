// Headless PRD §5 performance benchmark (--bench). Builds synthetic shows in code,
// drives the app's real frame (pollFrame + presentFrame: editor canvas, output
// frames, ImGui, editor swap, projector outputs) and reports JSON. It measures; it never
// asserts thresholds, because machines vary. The one exception is sameFrame, a
// correctness check: an edit missing from its own frame's projector image is reported
// as an error, which fails bench_smoke. See Bench.hpp for the flags.

#include "Bench.hpp"
#include "Application.hpp"
#include "EditorState.hpp"
#include "StressHarness.hpp"
#include "../core/GlObjectStats.hpp"
#include "../core/Logger.hpp"
#include "../output/OutputManager.hpp"
#include "../output/OutputWindow.hpp"
#include "../renderer/Framebuffer.hpp"
#include "../scene/Quad.hpp"
#include "../scene/QuadManager.hpp"
#include "../scene/Scene.hpp"
#include "../source/FrameData.hpp"
#include "../source/SourceManager.hpp"
#include "../source/TextureSource.hpp"
#include "../source/VideoFileSource.hpp"
#include "../source/procedural/ProceduralSource.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace quadmapper {

using json = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;

bool BenchConfig::wants(const std::string& scenario) const {
    if (scenarios == "all") return true;
    std::stringstream list(scenarios);
    std::string item;
    while (std::getline(list, item, ',')) {
        if (item == scenario) return true;
    }
    return false;
}

bool parseBenchArg(const char* arg, std::optional<BenchConfig>& cfg) {
    auto config = [&cfg]() -> BenchConfig& {
        if (!cfg) cfg = BenchConfig{};
        return *cfg;
    };
    auto value = [arg](const char* flag) -> const char* {
        const size_t n = std::strlen(flag);
        return std::strncmp(arg, flag, n) == 0 ? arg + n : nullptr;
    };
    if (std::strcmp(arg, "--bench") == 0) {
        config();
    } else if (const char* v = value("--bench=")) {
        config().scenarios = v;
    } else if ((v = value("--bench-out="))) {
        config().outPath = v;
    } else if ((v = value("--bench-seconds="))) {
        config().seconds = std::atof(v);
    } else if ((v = value("--bench-warmup="))) {
        config().warmupSeconds = std::atof(v);
    } else if ((v = value("--bench-outputs="))) {
        config().outputs = std::max(1, std::atoi(v));
    } else if ((v = value("--bench-monitor="))) {
        config().monitor = std::atoi(v);
    } else if ((v = value("--bench-vsync="))) {
        config().vsync = std::atoi(v) != 0;
    } else if ((v = value("--bench-quads="))) {
        config().quads = std::max(1, std::atoi(v));
    } else if ((v = value("--bench-videos="))) {
        config().videos = std::max(1, std::atoi(v));
    } else if ((v = value("--bench-media="))) {
        config().media = v;
    } else if ((v = value("--bench-hap-media="))) {
        config().hapMedia = v;
    } else if ((v = value("--bench-scenes="))) {
        config().scenes = std::clamp(std::atoi(v), 2, 9);  // taken with keys 1-9
    } else if ((v = value("--bench-switch-every="))) {
        config().switchEvery = std::max(2, std::atoi(v));
    } else if ((v = value("--bench-latency-samples="))) {
        config().latencySamples = std::max(1, std::atoi(v));
    } else if ((v = value("--bench-drift-seconds="))) {
        config().driftSeconds = std::atof(v);
    } else {
        return false;
    }
    return true;
}

namespace {

constexpr double FRAME_BUDGET_60HZ_MS = 1000.0 / 60.0;

double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

double seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

// Nearest-rank percentile of an ascending-sorted series.
double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const double rank = std::ceil(p * static_cast<double>(sorted.size()));
    const size_t index = static_cast<size_t>(std::max(1.0, rank)) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

json summarize(std::vector<double> values) {
    json j;
    j["n"] = values.size();
    if (values.empty()) return j;
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (double v : values) sum += v;
    j["mean"] = round3(sum / static_cast<double>(values.size()));
    j["p50"] = round3(percentile(values, 0.50));
    j["p95"] = round3(percentile(values, 0.95));
    j["p99"] = round3(percentile(values, 0.99));
    j["max"] = round3(values.back());
    return j;
}

// Nominal frame duration of a constant-frame-rate source: the container's rate when
// the source exposes it, else the shortest step between presented frames (a
// source that falls behind skips frames but never shortens a step).
double frameDuration(TextureSource* source, const std::vector<double>& deltas) {
    if (auto* video = dynamic_cast<VideoFileSource*>(source)) {
        const double fps = video->getVideoInfo().frameRate;
        if (fps > 0.0) return 1.0 / fps;
    }
    return deltas.empty() ? 0.0 : *std::min_element(deltas.begin(), deltas.end());
}

// Least-squares slope of y over x.
double slope(const std::vector<double>& x, const std::vector<double>& y) {
    const size_t n = std::min(x.size(), y.size());
    if (n < 2) return 0.0;
    double mx = 0.0, my = 0.0;
    for (size_t i = 0; i < n; ++i) { mx += x[i]; my += y[i]; }
    mx /= static_cast<double>(n);
    my /= static_cast<double>(n);
    double sxy = 0.0, sxx = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sxy += (x[i] - mx) * (y[i] - my);
        sxx += (x[i] - mx) * (x[i] - mx);
    }
    return sxx > 0.0 ? sxy / sxx : 0.0;
}

// CPU time this process has used (all threads, user + kernel), in seconds.
double processCpuSeconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    auto toSeconds = [](const FILETIME& t) {
        const ULONGLONG ticks = (static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        return static_cast<double>(ticks) * 1e-7;  // 100 ns units
    };
    return toSeconds(kernel) + toSeconds(user);
#else
    return 0.0;
#endif
}

json glCounts() {
    const auto s = GlObjectStats::Snapshot::capture();
    return json{{"textures", s.textures}, {"framebuffers", s.framebuffers},
                {"vertex_arrays", s.vertexArrays}, {"vertex_buffers", s.vertexBuffers},
                {"shader_programs", s.shaderPrograms}};
}

}  // namespace

// Friend of Application: drives its private frame halves and editor state.
class BenchRunner {
public:
    BenchRunner(Application& app, const BenchConfig& cfg) : app_(app), cfg_(cfg) {}
    int run();

private:
    struct Frame {
        Clock::time_point start, polled, rendered, editorSwapped, presented;
    };

    // Frame-time series of one measured run. period = presented-to-presented.
    struct FrameSeries {
        std::vector<double> period, editor, editorSwap, outputs;
        Clock::time_point first, last;
        bool any = false;

        void add(const Frame& f) {
            if (any) period.push_back(ms(last, f.presented));
            else first = f.presented;
            editor.push_back(ms(f.polled, f.rendered));
            editorSwap.push_back(ms(f.rendered, f.editorSwapped));
            outputs.push_back(ms(f.editorSwapped, f.presented));
            last = f.presented;
            any = true;
        }
        double fps() const {
            const double s = any ? seconds(first, last) : 0.0;
            return s > 0.0 ? static_cast<double>(period.size()) / s : 0.0;
        }
        size_t over(double budgetMs) const {
            return static_cast<size_t>(std::count_if(period.begin(), period.end(),
                [budgetMs](double p) { return p > budgetMs; }));
        }
        json toJson() const {
            json j;
            j["frames"] = period.size();
            j["fps"] = round3(fps());
            j["frame_ms"] = summarize(period);
            j["frames_over_16_7ms"] = over(FRAME_BUDGET_60HZ_MS);
            j["stage_ms"] = json{{"poll_to_rendered (editor canvas + output frames + "
                                  "source uploads + ImGui)", summarize(editor)},
                                 {"editor_swap", summarize(editorSwap)},
                                 {"projector_outputs (copy + swap)", summarize(outputs)}};
            return j;
        }
        // 60 FPS target: throughput >= 60 and 99% of frames inside the 16.7 ms budget.
        bool meets60() const {
            std::vector<double> sorted = period;
            std::sort(sorted.begin(), sorted.end());
            return fps() >= 59.5 && percentile(sorted, 0.99) <= FRAME_BUDGET_60HZ_MS;
        }
    };

    EditorState& es() { return *app_.m_editorState; }
    SourceManager& sources() { return *app_.m_sourceManager; }
    OutputManager& outputs() { return *app_.m_outputManager; }

    Frame step(const std::function<void()>& afterPoll = {});
    void runFrames(double secs, const std::function<void(const Frame&)>& perFrame = {});
    void resetShow();
    bool showOutputs(int count);
    void applyVsync();
    UUID outputId(int index) { return outputs().getOutputs()[static_cast<size_t>(index)]->getId(); }
    // Show a scene on output `index` (through a full-canvas slice).
    void route(const UUID& sceneId, int index) {
        const int at = findSceneIndex(es().scenes, sceneId);
        OutputManager::showScene(*outputs().getOutputs()[static_cast<size_t>(index)],
                                 at >= 0 ? es().scenes[static_cast<size_t>(at)].get() : nullptr,
                                 es().scenes);
    }
    GLFWkeyfun editorKeyCallback();
    std::vector<UUID> proceduralSet(int variant);
    void fillQuadGrid(Scene& scene, int count, const std::vector<UUID>& media, bool variety);
    json outputInfo(int count);

    json machine();
    json runQuads(int outputCount);
    json runVideos(const std::string& path, const char* name);
    json runSceneSwitch();
    json runLatency();
    json runDrift();
    json runSameFrame();
    json runPresent();
    int verifyReport();

    Application& app_;
    const BenchConfig& cfg_;
    bool aborted_ = false;
};

BenchRunner::Frame BenchRunner::step(const std::function<void()>& afterPoll) {
    Frame f;
    f.start = Clock::now();
    float dt = 0.0f;
    if (!app_.pollFrame(dt)) {
        aborted_ = true;  // the editor window was closed
    }
    f.polled = Clock::now();
    if (afterPoll) afterPoll();  // where OS input / a UI click lands in a real frame
    Application::FrameMarks marks;
    app_.presentFrame(dt, &marks);
    f.rendered = marks.rendered;
    f.editorSwapped = marks.editorSwapped;
    f.presented = marks.outputsPresented;
    return f;
}

void BenchRunner::runFrames(double secs, const std::function<void(const Frame&)>& perFrame) {
    const Clock::time_point begin = Clock::now();
    while (!aborted_ && seconds(begin, Clock::now()) < secs) {
        const Frame f = step();
        if (perFrame) perFrame(f);
    }
}

// Back to an empty show: one empty scene, no sources.
void BenchRunner::resetShow() {
    EditorState& e = es();
    e.commandHistory.clear();
    e.scenes.clear();
    e.setActiveScene(e.activeScene().getId());  // lazily recreates one scene
    for (auto& output : outputs().getOutputs()) {
        output->setSceneId(UUID::null());
        output->getSlices().clear();
    }
    e.quadTextures.clear();
    e.quadTexturePaths.clear();
    e.activeVideoSourceId = UUID::null();
    e.initialized = true;  // render() must not add its default quad
    sources().clear();
}

// Show `count` windowed 1920x1080 projector outputs (the real presentToOutputs path
// draws and swaps only visible outputs) and hide the rest.
bool BenchRunner::showOutputs(int count) {
    OutputManager& om = outputs();
    while (static_cast<int>(om.getOutputCount()) < count) {
        if (!om.createOutput("Bench " + std::to_string(om.getOutputCount() + 1))) {
            return false;
        }
    }
    int originX = 0, originY = 0;
    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    if (cfg_.monitor >= 0 && cfg_.monitor < monitorCount) {
        glfwGetMonitorPos(monitors[cfg_.monitor], &originX, &originY);
    }
    int index = 0;
    for (auto& output : om.getOutputs()) {
        GLFWwindow* window = output->getGLFWwindow();
        if (index < count && window) {
            glfwSetWindowSize(window, 1920, 1080);
            glfwSetWindowPos(window, originX + 40 * index, originY + 40 + 40 * index);
            output->show();
        } else {
            output->hide();
        }
        ++index;
    }
    return true;
}

// Uncapped mode: zero the swap interval of the editor and every shown output. The
// managers cache the interval they last set and only call glfwSwapInterval when
// their choice changes, so this override holds for the scenario. Call it after a
// frame has been presented (the first present sets the managers' intervals).
void BenchRunner::applyVsync() {
    if (cfg_.vsync) return;  // keep the app's own pacing
    GLFWwindow* previous = glfwGetCurrentContext();
    for (auto& output : outputs().getOutputs()) {
        if (output->isVisible()) {
            output->makeContextCurrent();
            glfwSwapInterval(0);
        }
    }
    glfwMakeContextCurrent(app_.m_window);
    glfwSwapInterval(0);
    glfwMakeContextCurrent(previous);
}

// The editor window's GLFW key callback (ImGui's, which chains to the app's): calling
// it is the path a real key press takes inside glfwPollEvents. Null if none is set.
GLFWkeyfun BenchRunner::editorKeyCallback() {
    GLFWwindow* window = app_.m_window;
    const GLFWkeyfun callback = glfwSetKeyCallback(window, nullptr);
    glfwSetKeyCallback(window, callback);
    return callback;
}

// Four full-HD procedural sources; `variant` picks which pattern leads, so scenes
// built from different variants look different.
std::vector<UUID> BenchRunner::proceduralSet(int variant) {
    static const ProceduralType kTypes[] = {ProceduralType::PLASMA, ProceduralType::TEST_PATTERN,
                                            ProceduralType::CHECKERBOARD, ProceduralType::GRADIENT};
    std::vector<UUID> ids;
    for (int i = 0; i < 4; ++i) {
        const UUID id = sources().createProceduralSource(kTypes[(i + variant) % 4], 1920, 1080);
        if (!id.isNull()) ids.push_back(id);
    }
    return ids;
}

// Lay `count` quads out on a grid covering the canvas, each slightly keystoned.
// With `variety`, a mix of features the renderer treats differently: mesh-edit
// quads with a warped control grid, bezier edges, colour correction, soft edges.
void BenchRunner::fillQuadGrid(Scene& scene, int count, const std::vector<UUID>& media,
                               bool variety) {
    const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = (count + cols - 1) / cols;
    const float w = 1.0f / static_cast<float>(cols);
    const float h = 1.0f / static_cast<float>(rows);
    QuadManager& qm = scene.getQuadManager();
    for (int i = 0; i < count; ++i) {
        const float x0 = static_cast<float>(i % cols) * w + 0.08f * w;
        const float y0 = static_cast<float>(i / cols) * h + 0.08f * h;
        const float x1 = x0 + 0.84f * w;
        const float y1 = y0 + 0.84f * h;
        const float skew = ((i % 3) - 1) * 0.06f * w;
        Quad* quad = qm.createQuad("Bench " + std::to_string(i + 1));
        QuadGeometry& g = quad->getGeometry();
        g.setCorners({Vec2(x0 + skew, y0), Vec2(x1 - skew, y0 + 0.04f * h),
                      Vec2(x1, y1), Vec2(x0, y1 - 0.04f * h)});
        if (!media.empty()) {
            quad->getTexture().mediaId = media[static_cast<size_t>(i) % media.size()];
        }
        if (!variety) continue;
        if (i % 4 == 1) {
            g.setMeshMode(MeshMode::MESH_EDIT);
            g.setUniformGrid(4);
            g.setMeshVertex(2, 2, Vec2(0.08f * w, 0.05f * h));
            g.setMeshVertex(1, 3, Vec2(-0.05f * w, 0.04f * h));
        } else if (i % 4 == 2) {
            g.setBezierEnabled(true);
            std::array<Vec2, 8> offsets;
            for (int k = 0; k < 8; ++k) {
                offsets[static_cast<size_t>(k)] =
                    Vec2((k % 2 ? 0.05f : -0.05f) * w, (k % 3 ? 0.04f : -0.04f) * h);
            }
            g.setBezierOffsets(offsets);
        }
        if (i % 3 == 0) {
            ColorCorrection cc;
            cc.brightness = 0.05f;
            cc.contrast = 1.2f;
            cc.gamma = 0.9f;
            cc.rgbGain = Vec3(1.1f, 1.0f, 0.9f);
            quad->setColorCorrection(cc);
        }
        if (i % 5 == 0) {
            SoftEdge se;
            se.left = se.right = 0.15f;
            se.gamma = 2.2f;
            quad->setSoftEdge(se);
        }
    }
}

json BenchRunner::outputInfo(int count) {
    json list = json::array();
    int index = 0;
    for (auto& output : outputs().getOutputs()) {
        if (index++ >= count) break;
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(output->getGLFWwindow(), &fbw, &fbh);
        list.push_back(json{{"name", output->getName()}, {"framebuffer", {fbw, fbh}},
                            {"visible", output->isVisible()}});
    }
    return list;
}

json BenchRunner::machine() {
    auto glString = [](GLenum name) {
        const char* s = reinterpret_cast<const char*>(glGetString(name));
        return std::string(s ? s : "unknown");
    };
    json monitors = json::array();
    int monitorCount = 0;
    GLFWmonitor** list = glfwGetMonitors(&monitorCount);
    for (int i = 0; i < monitorCount; ++i) {
        const GLFWvidmode* mode = glfwGetVideoMode(list[i]);
        const char* name = glfwGetMonitorName(list[i]);
        monitors.push_back(json{{"name", name ? name : "?"},
                                {"mode", mode ? json{mode->width, mode->height} : json()},
                                {"refresh_hz", mode ? mode->refreshRate : 0}});
    }
    return json{{"gl_vendor", glString(GL_VENDOR)}, {"gl_renderer", glString(GL_RENDERER)},
                {"gl_version", glString(GL_VERSION)},
                {"hardware_threads", std::thread::hardware_concurrency()},
                {"monitors", monitors}};
}

// (a) 256 quads per scene, one scene per output, all outputs 1080p.
json BenchRunner::runQuads(int outputCount) {
    json r;
    r["name"] = "quads" + std::to_string(cfg_.quads) + "_" + std::to_string(outputCount) + "out";
    r["target"] = "60 FPS with " + std::to_string(cfg_.quads) + " quads/scene on " +
                  std::to_string(outputCount) +
                  " x 1080p output(s): fps >= 60 and p99 frame <= 16.7 ms";
    resetShow();
    if (!showOutputs(outputCount)) return json{{"name", r["name"]}, {"error", "output creation failed"}};

    const std::vector<UUID> media = proceduralSet(0);
    std::vector<UUID> sceneIds;
    for (int o = 0; o < outputCount; ++o) {
        Scene& scene = (o == 0) ? es().activeScene()
                                : *es().addScene("Bench scene " + std::to_string(o + 1));
        fillQuadGrid(scene, cfg_.quads, media, /*variety=*/true);
        route(scene.getId(), o);
        sceneIds.push_back(scene.getId());
    }
    es().setActiveScene(sceneIds.front());
    es().quadManager().selectQuad(es().quadManager().getQuads().front()->getId());

    step();
    applyVsync();
    runFrames(cfg_.warmupSeconds);
    FrameSeries series;
    runFrames(cfg_.seconds, [&series](const Frame& f) { series.add(f); });

    r["quads_per_scene"] = cfg_.quads;
    // Each output's frame is drawn once and shown by both its window and its
    // preview (ADR-054).
    r["quads_drawn_per_frame"] = json{{"editor_canvas", cfg_.quads},
                                      {"output_frames", cfg_.quads * outputCount}};
    r["outputs"] = outputInfo(outputCount);
    r["result"] = series.toJson();
    r["pass"] = series.meets60();
    return r;
}

// (b) N simultaneous video sources, each its own decoder, one quad each.
json BenchRunner::runVideos(const std::string& path, const char* name) {
    json r;
    r["name"] = name;
    r["media"] = path;
    r["target"] = std::to_string(cfg_.videos) +
                  " simultaneous videos at 60 FPS output, no dropped source frames";
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) {
        r["skipped"] = "media not found";
        return r;
    }
    resetShow();
    if (!showOutputs(cfg_.outputs)) return json{{"name", name}, {"error", "output creation failed"}};

    struct Track {
        UUID id;
        TextureSource* source = nullptr;
        PlayableSource* playable = nullptr;
        uint64_t lastTs = std::numeric_limits<uint64_t>::max();
        long distinct = 0;
        long loops = 0;
        std::vector<double> deltas;  // seconds between consecutive distinct frames
        std::vector<double> lagMs;   // playhead minus presented frame time
    };
    std::vector<Track> tracks;
    std::vector<UUID> media;
    for (int i = 0; i < cfg_.videos; ++i) {
        Track t;
        t.id = sources().createVideoSource(path);
        t.source = sources().getSource(t.id);
        t.playable = sources().getSourceAs<PlayableSource>(t.id);
        if (t.id.isNull() || !t.playable) {
            r["error"] = "could not open source " + std::to_string(i + 1);
            return r;
        }
        t.playable->setLoopMode(LoopMode::LOOP);
        media.push_back(t.id);
        tracks.push_back(t);
    }
    for (auto& t : tracks) t.playable->play();  // start together

    // One scene per output showing the same video wall (one decode feeds both).
    std::vector<UUID> sceneIds;
    for (int o = 0; o < cfg_.outputs; ++o) {
        Scene& scene = (o == 0) ? es().activeScene()
                                : *es().addScene("Bench videos " + std::to_string(o + 1));
        fillQuadGrid(scene, cfg_.videos, media, /*variety=*/false);
        route(scene.getId(), o);
        sceneIds.push_back(scene.getId());
    }
    es().setActiveScene(sceneIds.front());

    step();
    applyVsync();
    runFrames(cfg_.warmupSeconds);

    for (auto& t : tracks) {
        const FrameData* fd = t.source->getCurrentFrame();
        t.lastTs = fd ? fd->timestamp : std::numeric_limits<uint64_t>::max();
    }
    FrameSeries series;
    const SourceManager::UploadStats uploadsBefore = sources().getUploadStats();
    const double cpuBefore = processCpuSeconds();
    const Clock::time_point begin = Clock::now();
    runFrames(cfg_.seconds, [&](const Frame& f) {
        series.add(f);
        for (auto& t : tracks) {
            const FrameData* fd = t.source->getCurrentFrame();
            if (!fd) continue;
            if (fd->timestamp != t.lastTs) {
                if (t.lastTs != std::numeric_limits<uint64_t>::max()) {
                    if (fd->timestamp > t.lastTs) {
                        t.deltas.push_back(static_cast<double>(fd->timestamp - t.lastTs) / 1e6);
                    } else {
                        ++t.loops;
                    }
                }
                ++t.distinct;
                t.lastTs = fd->timestamp;
            }
            const double lag = t.playable->getCurrentTime() * 1000.0 -
                               static_cast<double>(fd->timestamp) / 1000.0;
            if (lag > -100.0) t.lagMs.push_back(lag);  // skip the instant around a loop seam
        }
    });
    const double measured = seconds(begin, Clock::now());
    const double cpuSeconds = processCpuSeconds() - cpuBefore;
    const SourceManager::UploadStats& uploadsAfter = sources().getUploadStats();
    const double uploadMs = (uploadsAfter.seconds - uploadsBefore.seconds) * 1000.0;
    const uint64_t uploadCount = uploadsAfter.uploads - uploadsBefore.uploads;

    json perSource = json::array();
    long totalSkipped = 0;
    long totalPresented = 0;
    double totalExpected = 0.0;
    double worstRatio = std::numeric_limits<double>::max();
    std::vector<double> allLag;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const Track& t = tracks[i];
        const double frameDur = frameDuration(t.source, t.deltas);
        long skipped = 0;
        for (double d : t.deltas) {
            if (frameDur > 0.0) skipped += std::max(0L, std::lround(d / frameDur) - 1);
        }
        const double expected = frameDur > 0.0 ? measured / frameDur : 0.0;
        const double ratio = expected > 0.0 ? static_cast<double>(t.distinct) / expected : 0.0;
        totalSkipped += skipped;
        totalPresented += t.distinct;
        totalExpected += expected;
        worstRatio = std::min(worstRatio, ratio);
        allLag.insert(allLag.end(), t.lagMs.begin(), t.lagMs.end());
        perSource.push_back(json{{"source", static_cast<int>(i + 1)},
                                 {"size", {t.source->getWidth(), t.source->getHeight()}},
                                 {"fps", frameDur > 0.0 ? round3(1.0 / frameDur) : 0.0},
                                 {"presented_frames", t.distinct},
                                 {"expected_frames", round3(expected)},
                                 {"skipped_frames", skipped},
                                 {"loops", t.loops},
                                 {"decode_lag_ms", summarize(t.lagMs)}});
    }
    r["videos"] = cfg_.videos;
    // Where the frames went (ADR-055/056), counted at the end of the run.
    int hardwareCount = 0, zeroCopyCount = 0;
    for (const Track& t : tracks) {
        if (const auto* video = dynamic_cast<const VideoFileSource*>(t.source)) {
            hardwareCount += video->isHardwareDecoding() ? 1 : 0;
            zeroCopyCount += video->isZeroCopy() ? 1 : 0;
        }
    }
    r["decoding"] = json{{"hardware", hardwareCount}, {"zero_copy", zeroCopyCount}};
    r["outputs"] = outputInfo(cfg_.outputs);
    r["result"] = series.toJson();
    const double renderedFrames = static_cast<double>(series.period.size() + (series.any ? 1 : 0));
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    r["main_thread_uploads"] = json{
        {"uploads", uploadCount},
        {"ms_per_rendered_frame", renderedFrames > 0.0 ? round3(uploadMs / renderedFrames) : 0.0},
        {"ms_per_upload", uploadCount > 0 ? round3(uploadMs / static_cast<double>(uploadCount)) : 0.0}};
    r["process_cpu"] = json{
        {"cores_busy", measured > 0.0 ? round3(cpuSeconds / measured) : 0.0},
        {"percent_of_machine", measured > 0.0 ? round3(100.0 * cpuSeconds / (measured * threads)) : 0.0}};
    r["sources"] = json{{"skipped_frames_total", totalSkipped},
                        {"presented_frames_total", totalPresented},
                        {"expected_frames_total", round3(totalExpected)},
                        {"presented_over_expected_total",
                         totalExpected > 0.0 ? round3(totalPresented / totalExpected) : 0.0},
                        {"worst_presented_over_expected", round3(worstRatio)},
                        {"decode_lag_ms_all", summarize(allLag)},
                        {"per_source", perSource}};
    r["pass"] = series.meets60() && totalSkipped == 0 && worstRatio >= 0.98;
    return r;
}

// (c) Scene switch through the live-show path: the scene's number key, delivered
// through the editor window's key callback as a real press is and taken inside
// render() (US-008: scene N goes to the projector the active scene is on). Timed from
// the key press to the swap of that frame; sameFrame checks that the frame shows it.
json BenchRunner::runSceneSwitch() {
    json r;
    r["name"] = "sceneSwitch";
    r["target"] = "scene switch < 16 ms (scene key press -> first presented output frame)";
    resetShow();
    if (!showOutputs(cfg_.outputs)) return json{{"name", "sceneSwitch"}, {"error", "output creation failed"}};

    const int quadsPerScene = std::max(1, cfg_.quads / 4);
    std::vector<UUID> sceneIds;
    for (int s = 0; s < cfg_.scenes; ++s) {
        Scene& scene = (s == 0) ? es().activeScene()
                                : *es().addScene("Bench scene " + std::to_string(s + 1));
        fillQuadGrid(scene, quadsPerScene, proceduralSet(s), /*variety=*/true);
        sceneIds.push_back(scene.getId());
    }
    // Output 0 is the switched projector; any other output keeps one fixed scene.
    es().setActiveScene(sceneIds.front());
    route(sceneIds.front(), 0);
    for (int o = 1; o < cfg_.outputs; ++o) {
        Scene* fixed = es().addScene("Bench fixed " + std::to_string(o + 1));
        fillQuadGrid(*fixed, quadsPerScene, proceduralSet(o), true);
        route(fixed->getId(), o);
    }
    es().setActiveScene(sceneIds.front());
    const UUID projector = outputId(0);
    GLFWwindow* window = app_.m_window;
    const GLFWkeyfun keyCallback = editorKeyCallback();
    if (!keyCallback) {
        r["error"] = "no key callback on the editor window";
        return r;
    }

    step();
    applyVsync();
    runFrames(cfg_.warmupSeconds);

    std::vector<double> toOutput, toEditor, switchFrame, otherFrame;
    long wrongScene = 0;
    size_t current = 0;
    Clock::time_point lastPresented = Clock::now();
    const Clock::time_point begin = Clock::now();
    int frame = 0;
    while (!aborted_ && seconds(begin, Clock::now()) < cfg_.seconds) {
        const bool doSwitch = (++frame % cfg_.switchEvery) == 0;
        Clock::time_point requested;
        const size_t next = (current + 1) % sceneIds.size();
        const int key = GLFW_KEY_1 + static_cast<int>(next);  // scene next+1
        const Frame f = step([&]() {
            if (!doSwitch) return;
            requested = Clock::now();
            keyCallback(window, key, glfwGetKeyScancode(key), GLFW_PRESS, 0);
        });
        if (doSwitch) keyCallback(window, key, glfwGetKeyScancode(key), GLFW_RELEASE, 0);
        const double period = ms(lastPresented, f.presented);
        lastPresented = f.presented;
        if (!doSwitch) {
            otherFrame.push_back(period);
            continue;
        }
        current = next;
        const Scene* shown = OutputManager::sceneForOutput(es().scenes, *outputs().getOutput(projector));
        if (!shown || shown->getId() != sceneIds[current]) ++wrongScene;
        toOutput.push_back(ms(requested, f.presented));
        toEditor.push_back(ms(requested, f.editorSwapped));
        switchFrame.push_back(period);
    }

    r["scenes"] = cfg_.scenes;
    r["quads_per_scene"] = quadsPerScene;
    r["switches"] = toOutput.size();
    r["switch_to_output_present_ms"] = summarize(toOutput);
    r["switch_to_editor_present_ms"] = summarize(toEditor);
    r["frame_ms_on_switch"] = summarize(switchFrame);
    r["frame_ms_otherwise"] = summarize(otherFrame);
    r["switches_not_shown_on_output"] = wrongScene;
    std::vector<double> sorted = toOutput;
    std::sort(sorted.begin(), sorted.end());
    r["pass"] = !sorted.empty() && wrongScene == 0 && percentile(sorted, 0.95) < 16.0;
    return r;
}

// (d) Input-to-display latency: a vertex nudge injected through the editor
// window's GLFW key callback (the path a real key press takes), timed to the
// SwapBuffers return of the first frame that draws the moved vertex.
json BenchRunner::runLatency() {
    json r;
    r["name"] = "latency";
    r["target"] = "input-to-display < 50 ms (p95)";
    r["measures"] = "key event delivered to the app (steady_clock t0) -> glfwSwapBuffers "
                    "returns for the first frame containing the nudged vertex";
    r["excludes"] = "OS/USB input delay before the event reaches GLFW, frames queued in the "
                    "driver/DWM after SwapBuffers returns, display scanout and panel response";
    resetShow();
    if (!showOutputs(cfg_.outputs)) return json{{"name", "latency"}, {"error", "output creation failed"}};

    Scene& scene = es().activeScene();
    fillQuadGrid(scene, 16, proceduralSet(0), /*variety=*/false);
    route(scene.getId(), 0);
    for (int o = 1; o < cfg_.outputs; ++o) {
        Scene* other = es().addScene("Bench other " + std::to_string(o + 1));
        fillQuadGrid(*other, 16, proceduralSet(o), false);
        route(other->getId(), o);
    }
    es().setActiveScene(scene.getId());
    Quad* quad = scene.getQuadManager().getQuads().front().get();
    scene.getQuadManager().selectQuad(quad->getId());
    es().currentSelectionType = EditorState::SelectionType::Quad;

    GLFWwindow* window = app_.m_window;
    const GLFWkeyfun keyCallback = editorKeyCallback();
    if (!keyCallback) {
        r["error"] = "no key callback on the editor window";
        return r;
    }
    auto key = [&](int glfwKey, int action) {
        keyCallback(window, glfwKey, glfwGetKeyScancode(glfwKey), action, 0);
    };

    step();
    applyVsync();
    runFrames(cfg_.warmupSeconds);

    // Tab selects the first corner, through the same key path.
    step([&]() { key(GLFW_KEY_TAB, GLFW_PRESS); });
    step();
    step([&]() { key(GLFW_KEY_TAB, GLFW_RELEASE); });
    for (int i = 0; i < 3; ++i) step();
    if (es().cornerSelectedVertex < 0) {
        r["error"] = "Tab did not select a vertex (the canvas did not own the keyboard)";
        return r;
    }
    const int vertex = es().cornerSelectedVertex;

    std::vector<double> toOutput, toEditor, frames;
    long missed = 0;
    for (int s = 0; s < cfg_.latencySamples && !aborted_; ++s) {
        const int arrow = (s % 2 == 0) ? GLFW_KEY_RIGHT : GLFW_KEY_LEFT;
        const Vec2 before = quad->getGeometry().getCorners()[static_cast<size_t>(vertex)];
        Clock::time_point t0;
        Frame f = step([&]() {
            t0 = Clock::now();
            key(arrow, GLFW_PRESS);
        });
        bool seen = false;
        for (int n = 0; n < 10 && !aborted_; ++n) {
            if (quad->getGeometry().getCorners()[static_cast<size_t>(vertex)] != before) {
                toOutput.push_back(ms(t0, f.presented));
                toEditor.push_back(ms(t0, f.editorSwapped));
                frames.push_back(static_cast<double>(n + 1));
                seen = true;
                break;
            }
            f = step([&]() { if (n == 1) key(arrow, GLFW_RELEASE); });
        }
        if (!seen) ++missed;
        key(arrow, GLFW_RELEASE);
        for (int n = 0; n < 4; ++n) step();  // settle between samples
    }

    r["samples"] = toOutput.size();
    r["missed"] = missed;
    r["vsync"] = cfg_.vsync;
    r["key_to_output_swap_ms"] = summarize(toOutput);
    r["key_to_editor_swap_ms"] = summarize(toEditor);
    r["frames_until_visible"] = summarize(frames);
    std::vector<double> sorted = toOutput;
    std::sort(sorted.begin(), sorted.end());
    r["pass"] = !sorted.empty() && missed == 0 && percentile(sorted, 0.95) < 50.0;
    return r;
}

// (e) Playback drift: presented media time against the wall clock over a long run.
json BenchRunner::runDrift() {
    json r;
    r["name"] = "drift";
    r["media"] = cfg_.media;
    r["target"] = "video sync drift < 1 frame per hour (media time vs wall clock)";
    std::error_code ec;
    if (!std::filesystem::exists(cfg_.media, ec)) {
        r["skipped"] = "media not found";
        return r;
    }
    resetShow();
    if (!showOutputs(1)) return json{{"name", "drift"}, {"error", "output creation failed"}};

    const UUID id = sources().createVideoSource(cfg_.media);
    TextureSource* source = sources().getSource(id);
    PlayableSource* playable = sources().getSourceAs<PlayableSource>(id);
    if (!playable) {
        r["error"] = "could not open media";
        return r;
    }
    playable->setLoopMode(LoopMode::LOOP);
    Scene& scene = es().activeScene();
    Quad* quad = scene.getQuadManager().createQuad("Bench video");
    quad->getGeometry().setCorners({Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f),
                                    Vec2(0.0f, 1.0f)});
    quad->getTexture().mediaId = id;
    route(scene.getId(), 0);

    step();
    applyVsync();
    const double duration = playable->getDuration();
    playable->play();

    // Let playback settle (decoder start-up), then start both clocks together.
    runFrames(std::max(1.0, cfg_.warmupSeconds));
    const FrameData* fd = source->getCurrentFrame();
    if (!fd || duration <= 0.0) {
        r["error"] = "no frame decoded / unknown duration";
        return r;
    }

    const Clock::time_point wall0 = Clock::now();
    const double pts0 = static_cast<double>(fd->timestamp) / 1e6;
    const double pos0 = playable->getCurrentTime();
    double lastPts = pts0, lastPos = pos0;
    long ptsLoops = 0, posLoops = 0;
    std::vector<double> wall, presentedDrift, playheadDrift, deltas, seamLossMs, loopFrameMs;
    Clock::time_point lastPresented = wall0;
    double lastPlayheadDrift = 0.0;
    FrameSeries series;
    while (!aborted_ && seconds(wall0, Clock::now()) < cfg_.driftSeconds) {
        const Frame f = step();
        series.add(f);
        const double t = seconds(wall0, f.presented);
        fd = source->getCurrentFrame();
        const double pts = fd ? static_cast<double>(fd->timestamp) / 1e6 : lastPts;
        if (pts < lastPts - 0.5 * duration) ++ptsLoops;
        else if (pts > lastPts) deltas.push_back(pts - lastPts);
        lastPts = pts;
        const double pos = playable->getCurrentTime();
        const bool looped = pos < lastPos - 0.5 * duration;
        if (looped) ++posLoops;
        lastPos = pos;

        const double drift = (static_cast<double>(posLoops) * duration + pos - pos0) - t;
        if (looped) {
            seamLossMs.push_back((lastPlayheadDrift - drift) * 1000.0);
            loopFrameMs.push_back(ms(lastPresented, f.presented));
        }
        lastPlayheadDrift = drift;
        lastPresented = f.presented;
        wall.push_back(t);
        playheadDrift.push_back(drift);
        presentedDrift.push_back((static_cast<double>(ptsLoops) * duration + pts - pts0) - t);
    }

    const double frameDur = frameDuration(source, deltas);
    const double fps = frameDur > 0.0 ? 1.0 / frameDur : 0.0;
    const double presentedSlope = slope(wall, presentedDrift);  // s of drift per s
    const double playheadSlope = slope(wall, playheadDrift);
    const double elapsed = wall.empty() ? 0.0 : wall.back();
    r["played_seconds"] = round3(elapsed);
    r["clip_duration_s"] = round3(duration);
    r["clip_fps"] = round3(fps);
    r["loops"] = posLoops;
    r["presented_drift_end_ms"] = presentedDrift.empty() ? 0.0 : round3(presentedDrift.back() * 1000.0);
    r["playhead_drift_end_ms"] = playheadDrift.empty() ? 0.0 : round3(playheadDrift.back() * 1000.0);
    r["presented_drift_frames_per_hour"] = round3(presentedSlope * 3600.0 * fps);
    r["playhead_drift_frames_per_hour"] = round3(playheadSlope * 3600.0 * fps);
    r["loop_seam_loss_ms"] = summarize(seamLossMs);
    r["frame_ms_at_loop_seam"] = summarize(loopFrameMs);
    r["result"] = series.toJson();
    r["pass"] = elapsed > 0.0 && std::fabs(presentedSlope * 3600.0 * fps) < 1.0;
    return r;
}

// (f) Same-frame edits, a correctness check rather than a measurement: an edit made
// during render() must be in the projector frame drawn in that same frame. The
// projector's frame is read back right after the frame that made the edit, for a scene
// take by its number key (taken inside render(), as a real press is) and for a quad
// opacity change made where the panels make theirs (the Inspector's slider). Any miss
// is an error, so bench_smoke fails.
json BenchRunner::runSameFrame() {
    json r;
    r["name"] = "sameFrame";
    r["target"] = "an edit made during a frame is in that frame's projector image";
    resetShow();
    if (!showOutputs(1)) return json{{"name", "sameFrame"}, {"error", "output creation failed"}};

    // Scene 1, on the projector: one plain (white) quad over the whole canvas.
    // Scene 2: empty, so the projector is black.
    Scene& lit = es().activeScene();
    Quad* quad = lit.getQuadManager().createQuad("Bench white");
    quad->getGeometry().setCorners({Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f),
                                    Vec2(0.0f, 1.0f)});
    es().addScene("Bench black");
    const UUID projector = outputId(0);
    route(lit.getId(), 0);
    es().setActiveScene(lit.getId());
    GLFWwindow* window = app_.m_window;
    const GLFWkeyfun keyCallback = editorKeyCallback();
    if (!keyCallback) {
        r["error"] = "no key callback on the editor window";
        return r;
    }

    // The centre pixel (RGBA) of the projector's current frame; -1s if unreadable.
    Framebuffer probe;
    FrameCopy copy;
    auto centre = [&]() {
        std::array<int, 4> rgba{-1, -1, -1, -1};
        const Framebuffer* frame = outputs().outputFrame(projector);
        if (!frame || !probe.resize(frame->width(), frame->height())) return rgba;
        GLint viewport[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, viewport);
        const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        probe.bind();
        if (copy.copy(*frame, frame->width(), frame->height())) {
            unsigned char px[4] = {0, 0, 0, 0};
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(frame->width() / 2, frame->height() / 2, 1, 1, GL_RGBA,
                         GL_UNSIGNED_BYTE, px);
            rgba = {px[0], px[1], px[2], px[3]};
        }
        Framebuffer::unbind();
        if (scissor) glEnable(GL_SCISSOR_TEST);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        return rgba;
    };
    auto isGrey = [](const std::array<int, 4>& px, int value) {
        return std::abs(px[0] - value) <= 2 && std::abs(px[1] - value) <= 2 &&
               std::abs(px[2] - value) <= 2;
    };
    // Scene N by its key, pressed where OS input lands; the frame that takes it.
    auto takeScene = [&](int number, Clock::time_point& pressed) {
        const int key = GLFW_KEY_0 + number;
        const Frame f = step([&]() {
            pressed = Clock::now();
            keyCallback(window, key, glfwGetKeyScancode(key), GLFW_PRESS, 0);
        });
        keyCallback(window, key, glfwGetKeyScancode(key), GLFW_RELEASE, 0);
        return f;
    };
    // One frame whose render() sets the quad's opacity where the panels act.
    auto opacityFrame = [&](float opacity) {
        app_.m_benchInFrameEdit = [quad, opacity]() { quad->setOpacity(opacity); };
        step();
        app_.m_benchInFrameEdit = nullptr;
    };

    step();
    applyVsync();
    for (int i = 0; i < 3; ++i) step();
    const std::array<int, 4> white = centre();
    if (white[0] < 200 || !isGrey(white, white[0])) {
        r["error"] = "the lit scene's quad is not in the projector frame";
        return r;
    }

    const int rounds = 3;
    long takesMissed = 0, opacityMissed = 0, notOpaque = 0;
    std::vector<double> takeToOutput;
    for (int i = 0; i < rounds && !aborted_; ++i) {
        for (int number : {2, 1}) {
            Clock::time_point pressed;
            const Frame f = takeScene(number, pressed);
            const std::array<int, 4> px = centre();
            if (isGrey(px, number == 2 ? 0 : white[0])) {
                takeToOutput.push_back(ms(pressed, f.presented));
            } else {
                ++takesMissed;
            }
        }
        for (float opacity : {0.5f, 1.0f}) {
            opacityFrame(opacity);
            const std::array<int, 4> px = centre();
            if (!isGrey(px, static_cast<int>(std::lround(white[0] * opacity)))) ++opacityMissed;
            if (px[3] != 255) ++notOpaque;  // the preview draws the frame blended by alpha
        }
    }

    r["rounds"] = rounds;
    r["scene_takes_missing_their_frame"] = takesMissed;
    r["opacity_edits_missing_their_frame"] = opacityMissed;
    r["frame_pixels_not_opaque"] = notOpaque;
    r["take_key_to_output_present_ms"] = summarize(takeToOutput);
    const bool ok = !aborted_ && takesMissed == 0 && opacityMissed == 0 && notOpaque == 0;
    if (!ok) {
        r["error"] = "an edit made during a frame was not in that frame's projector image "
                     "(or the frame was not opaque)";
    }
    r["pass"] = ok;
    return r;
}

namespace {

// The desktop compositor's counters (Windows DWM, the whole desktop), or null.
json dwmTiming() {
#ifdef _WIN32
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info))) return json();
    const double rate = info.rateRefresh.uiDenominator
                            ? static_cast<double>(info.rateRefresh.uiNumerator) / info.rateRefresh.uiDenominator
                            : 0.0;
    return json{{"refresh_hz", round3(rate)}, {"refresh_count", info.cRefresh},
                {"frames_displayed", info.cFramesDisplayed}, {"frames_dropped", info.cFramesDropped},
                {"frames_missed", info.cFramesMissed}, {"frame", info.cFrame}};
#else
    return json();
#endif
}

}  // namespace

// TD-12 (ADR-047 §6): how each projector output presents. Every output goes fullscreen
// on its own monitor (as perform mode does, borderless or exclusive) with the app's
// own vsync pacing: the first output holds vsync (interval 1), the rest swap with
// interval 0. For each output: the time between its swaps (after SwapBuffers returns),
// a 1 ms histogram, and presents per refresh of the monitor it is on (1 = one new
// frame each refresh; < 1 = frames repeat, uneven motion; > 1 = more frames than the
// display can show, dropped or, where the swap is not composited, torn). The DWM
// counters (the compositor, whole desktop) are sampled around each case. Tearing
// itself cannot be seen from software; the configuration that can tear (interval 0
// on an output the compositor does not compose) is reported.
json BenchRunner::runPresent() {
    json r;
    r["name"] = "present";
    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    const int count = std::max(2, cfg_.outputs);
    if (monitorCount < 1) {
        r["skipped"] = "no monitor";
        return r;
    }
    resetShow();
    if (!showOutputs(count)) {
        r["error"] = "could not create output windows";
        return r;
    }
    Scene& scene = es().activeScene();
    fillQuadGrid(scene, 64, proceduralSet(0), true);
    for (int o = 0; o < count; ++o) route(scene.getId(), o);

    struct Case {
        const char* name;
        bool exclusive;
        bool reversed;  // the outputs' monitors in reverse order: the other one holds vsync
    };
    const Case cases[] = {{"borderless, vsync held by the output on monitor 0", false, false},
                          {"borderless, vsync held by the output on the last monitor", false, true},
                          {"exclusive, vsync held by the output on monitor 0", true, false},
                          {"exclusive, vsync held by the output on the last monitor", true, true}};
    json results = json::array();
    for (const Case& c : cases) {
        std::vector<int> monitorOf(static_cast<size_t>(count));
        for (int o = 0; o < count; ++o) {
            const int m = o % monitorCount;
            monitorOf[static_cast<size_t>(o)] = c.reversed ? monitorCount - 1 - m : m;
        }
        int o = 0;
        for (auto& output : outputs().getOutputs()) {
            if (o >= count) break;
            const int m = monitorOf[static_cast<size_t>(o++)];
            output->show();
            const auto done = c.exclusive ? output->goFullscreen(m) : output->goBorderlessFullscreen(m);
            if (!done) Logger::warning("bench present: {}", done.error().message);
        }
        const Clock::time_point warm = Clock::now();
        while (!aborted_ && seconds(warm, Clock::now()) < std::max(1.0, cfg_.warmupSeconds)) step();

        std::unordered_map<UUID, std::vector<Clock::time_point>> swaps;
        std::unordered_map<UUID, int> intervals;
        std::vector<double> loop;
        Clock::time_point lastLoop{};
        const json dwmBefore = dwmTiming();
        const Clock::time_point begin = Clock::now();
        while (!aborted_ && seconds(begin, Clock::now()) < cfg_.seconds) {
            const Frame f = step();
            if (lastLoop != Clock::time_point{}) loop.push_back(ms(lastLoop, f.presented));
            lastLoop = f.presented;
            for (const auto& stamp : outputs().lastPresents()) {
                swaps[stamp.outputId].push_back(stamp.swapped);
                intervals[stamp.outputId] = stamp.swapInterval;
            }
        }
        const double elapsed = seconds(begin, Clock::now());
        const json dwmAfter = dwmTiming();

        json perOutput = json::array();
        o = 0;
        for (auto& output : outputs().getOutputs()) {
            if (o >= count) break;
            const int m = monitorOf[static_cast<size_t>(o++)];
            const GLFWvidmode* mode = glfwGetVideoMode(monitors[m]);
            const char* monitorName = glfwGetMonitorName(monitors[m]);
            const double refresh = mode ? mode->refreshRate : 0.0;
            const std::vector<Clock::time_point>& t = swaps[output->getId()];
            std::vector<double> gaps;
            for (size_t i = 1; i < t.size(); ++i) gaps.push_back(ms(t[i - 1], t[i]));
            std::vector<int> histogram(41, 0);  // 1 ms bins; the last one is >= 40 ms
            for (double g : gaps) histogram[static_cast<size_t>(std::min(40.0, std::floor(g)))]++;
            const double presentsPerSecond = static_cast<double>(t.size()) / elapsed;
            // A swap that returns within 1.5 ms of the previous one is a present the
            // display could not have shown on its own refresh (interval 0 bursts).
            const auto early = std::count_if(gaps.begin(), gaps.end(), [&](double g) {
                return refresh > 0.0 && g < 0.5 * 1000.0 / refresh;
            });
            perOutput.push_back(json{
                {"output", output->getName()},
                {"monitor", m},
                {"monitor_name", monitorName ? monitorName : "?"},
                {"monitor_mode", mode ? json{mode->width, mode->height} : json()},
                {"refresh_hz", refresh},
                {"mode", c.exclusive ? "exclusive" : "borderless"},
                {"swap_interval", intervals.count(output->getId()) ? intervals[output->getId()] : -1},
                {"presents", t.size()},
                {"presents_per_second", round3(presentsPerSecond)},
                {"presents_per_refresh", refresh > 0.0 ? round3(presentsPerSecond / refresh) : 0.0},
                {"swap_to_swap_ms", summarize(gaps)},
                {"swaps_closer_than_half_a_refresh", early},
                {"histogram_1ms", histogram}});
        }
        json dwm;
        if (!dwmBefore.is_null() && !dwmAfter.is_null()) {
            auto delta = [&](const char* key) {
                return dwmAfter[key].get<double>() - dwmBefore[key].get<double>();
            };
            dwm = json{{"refresh_hz", dwmAfter["refresh_hz"]}, {"refreshes", delta("refresh_count")},
                       {"frames_displayed", delta("frames_displayed")},
                       {"frames_dropped", delta("frames_dropped")}, {"frames_missed", delta("frames_missed")}};
        }
        results.push_back(json{{"case", c.name}, {"seconds", round3(elapsed)},
                               {"loop_frame_ms", summarize(loop)}, {"outputs", perOutput},
                               {"dwm", dwm}});
        for (auto& output : outputs().getOutputs()) {
            output->goWindowed();
        }
        for (int i = 0; i < 10 && !aborted_; ++i) step();
    }
    showOutputs(0);
    r["monitors"] = monitorCount;
    r["cases"] = results;
    r["pass"] = !aborted_;
    return r;
}

int BenchRunner::run() {
    EditorState& e = es();
    // Deterministic and side-effect free: default dock layout (no imgui.ini read or
    // written), no autosave, no recovery prompt, no default quad.
    ImGui::GetIO().IniFilename = nullptr;
    app_.m_autoSaveEnabled = false;
    app_.m_haveCleanBaseline = true;
    e.initialized = true;

    json report;
    report["bench"] = "QUADmapper PRD 5 performance benchmark";
#if defined(_DEBUG)
    report["build"] = "Debug";
#else
    report["build"] = "Release";
#endif
    report["machine"] = machine();
    report["config"] = json{{"scenarios", cfg_.scenarios}, {"seconds", cfg_.seconds},
                            {"warmup_seconds", cfg_.warmupSeconds}, {"outputs", cfg_.outputs},
                            {"monitor", cfg_.monitor}, {"vsync", cfg_.vsync}, {"quads", cfg_.quads}, {"videos", cfg_.videos},
                            {"media", cfg_.media}, {"hap_media", cfg_.hapMedia},
                            {"scenes", cfg_.scenes}, {"switch_every", cfg_.switchEvery},
                            {"latency_samples", cfg_.latencySamples},
                            {"drift_seconds", cfg_.driftSeconds}};

    // Leak baseline: an empty scene on every output window, each drawn once (the
    // output frames are created lazily on the first frame).
    const int leakOutputs = std::max(2, cfg_.outputs);
    auto emptyShow = [&]() {
        resetShow();
        if (!showOutputs(leakOutputs)) return false;
        route(es().activeScene().getId(), 0);
        for (int o = 1; o < leakOutputs; ++o) {
            route(es().addScene("Bench empty")->getId(), o);
        }
        return true;
    };
    if (!emptyShow()) {
        std::fprintf(stderr, "bench: could not create output windows\n");
        return 2;
    }
    for (int i = 0; i < 10; ++i) step();
    const json glBefore = glCounts();
    const std::size_t wsBefore = processWorkingSetBytes();

    json scenarios = json::array();
    auto record = [&](json result) {
        Logger::info("[BENCH] {}", result.dump());
        scenarios.push_back(std::move(result));
    };
    if (cfg_.wants("quads256")) {
        record(runQuads(1));
        if (cfg_.outputs > 1) record(runQuads(cfg_.outputs));
    }
    if (cfg_.wants("videos16")) {
        record(runVideos(cfg_.media, "videos16"));
        if (!cfg_.hapMedia.empty()) record(runVideos(cfg_.hapMedia, "videos16_hap"));
    }
    if (cfg_.wants("sceneSwitch")) record(runSceneSwitch());
    if (cfg_.wants("latency")) record(runLatency());
    if (cfg_.wants("drift")) record(runDrift());
    if (cfg_.wants("sameFrame")) record(runSameFrame());
    // Takes over every monitor (fullscreen), so never part of "all".
    if (cfg_.scenarios != "all" && cfg_.wants("present")) record(runPresent());
    report["scenarios"] = scenarios;

    // Renderers drop a deleted quad's GPU mesh after 600 idle scene passes (one or
    // more per frame for every renderer drawing a scene), so settle past that.
    emptyShow();
    for (int i = 0; i < 650 && !aborted_; ++i) step();
    const json glAfter = glCounts();
    const std::size_t wsAfter = processWorkingSetBytes();
    report["leaks"] = json{
        {"note", "informational: GL objects and working set after every scenario was torn "
                 "down, against the same empty show before; the stress_smoke ctest is the gate"},
        {"gl_before", glBefore}, {"gl_after", glAfter}, {"gl_flat", glBefore == glAfter},
        {"working_set_mb_before", round3(static_cast<double>(wsBefore) / (1024.0 * 1024.0))},
        {"working_set_mb_after", round3(static_cast<double>(wsAfter) / (1024.0 * 1024.0))}};
    report["aborted"] = aborted_;

    const std::string text = report.dump(2);
    std::cout << text << std::endl;
    {
        std::ofstream file(cfg_.outPath, std::ios::binary | std::ios::trunc);
        file << text << '\n';
        if (!file) {
            std::fprintf(stderr, "bench: could not write %s\n", cfg_.outPath.c_str());
            return 2;
        }
    }
    if (aborted_) return 3;
    return verifyReport();
}

// Read the written report back and check its shape: the exit code then says whether
// the harness ran (bench_smoke), never whether a performance target was met.
int BenchRunner::verifyReport() {
    std::ifstream file(cfg_.outPath, std::ios::binary);
    const json parsed = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.contains("scenarios") ||
        !parsed["scenarios"].is_array() || parsed["scenarios"].empty()) {
        std::fprintf(stderr, "bench: %s is not a valid report\n", cfg_.outPath.c_str());
        return 4;
    }
    for (const auto& scenario : parsed["scenarios"]) {
        const bool ran = scenario.contains("pass") && scenario["pass"].is_boolean();
        if (scenario.contains("error") || (!ran && !scenario.contains("skipped"))) {
            std::fprintf(stderr, "bench: scenario %s failed to run: %s\n",
                         scenario.value("name", std::string("?")).c_str(),
                         scenario.value("error", std::string("no result")).c_str());
            return 5;
        }
    }
    return 0;
}

int Application::runBench(const BenchConfig& cfg) {
    BenchRunner runner(*this, cfg);
    return runner.run();
}

}  // namespace quadmapper
