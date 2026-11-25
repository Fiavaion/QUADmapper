#pragma once

// Video export (owner 2026-11-23, ADR-060): render the show to one H.264 MP4 per
// output, to play from a standalone media player without the PC. This header holds
// the request, the progress the UI reads, and the pure rules (length, file names,
// audio sample ranges), GL-free so they are unit-tested. The job itself is
// app/VideoExport.

#include "scene/Scene.hpp"
#include "scene/QuadManager.hpp"
#include "core/UUID.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace quadmapper {

struct VideoExportRequest {
    std::filesystem::path folder;    // where the files go
    std::string baseName;            // "<baseName> - <output name>.mp4"
    std::vector<UUID> outputIds;     // the outputs to export, one file each
    int fps = 30;
    double seconds = 10.0;           // length of every file
    bool audio = true;               // the mapped videos' sound (AAC)
};

struct VideoExportStatus {
    bool active = false;             // an export is preparing, rendering or finishing
    bool finished = false;           // the last export ended (done, failed or cancelled)
    bool failed = false;
    bool cancelled = false;
    float progress = 0.0f;           // 0..1 while active
    std::string message;             // what it is doing, or why it failed
    std::vector<std::filesystem::path> written;  // files of the last export
};

// Media ids bound to the quads and objects of the scenes in `sceneIds`.
inline std::unordered_set<UUID> mediaUsedByScenes(const std::vector<std::unique_ptr<Scene>>& scenes,
                                                  const std::unordered_set<UUID>& sceneIds) {
    std::unordered_set<UUID> ids;
    for (const auto& scene : scenes) {
        if (!scene || sceneIds.count(scene->getId()) == 0) continue;
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            if (!quad->getTexture().mediaId.isNull()) ids.insert(quad->getTexture().mediaId);
        }
        for (const auto& object : scene->getObjects()) {
            if (!object->getTexture().mediaId.isNull()) ids.insert(object->getTexture().mediaId);
        }
    }
    return ids;
}

// The export length when exactly one video is mapped (the owner's rule): that video's
// playing length, its duration at its speed. `videoSeconds(id)` gives a source's
// playing length, or nullopt when it is not a video (an image, a generator, missing).
// nullopt when no video or several distinct videos are mapped: the user sets it.
template <typename VideoSeconds>
std::optional<double> singleVideoSeconds(const std::unordered_set<UUID>& mediaIds, VideoSeconds videoSeconds) {
    std::optional<double> found;
    int videos = 0;
    for (const UUID& id : mediaIds) {
        const std::optional<double> seconds = videoSeconds(id);
        if (!seconds || !(*seconds > 0.0)) continue;
        ++videos;
        found = seconds;
    }
    if (videos != 1) return std::nullopt;
    return found;
}

// A video's playing length: duration / |speed|, or nullopt for no duration or a
// stopped speed.
inline std::optional<double> playingSeconds(double duration, double speed) {
    if (!(duration > 0.0) || !(std::abs(speed) > 1e-6)) return std::nullopt;
    return duration / std::abs(speed);
}

inline int exportFrameCount(double seconds, int fps) {
    if (!(seconds > 0.0) || fps <= 0) return 0;
    return std::max(1, static_cast<int>(std::llround(seconds * fps)));
}

// The audio sample frames that go with video frame `frame`: [first, first + count).
// Rounded per frame from the start, so the sound never drifts from the picture.
struct SampleRange {
    int64_t first = 0;
    int64_t count = 0;
};
inline SampleRange audioSamplesForFrame(int64_t frame, int fps, int sampleRate) {
    auto at = [&](int64_t f) {
        return static_cast<int64_t>(std::llround(static_cast<double>(f) * sampleRate / fps));
    };
    const int64_t first = at(frame);
    return {first, at(frame + 1) - first};
}

// A file name's characters that Windows refuses, replaced by '_'; trailing dots and
// spaces dropped; never empty.
inline std::string safeFileNamePart(const std::string& text) {
    std::string out;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool bad = u < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
                         c == '\\' || c == '|' || c == '?' || c == '*';
        out += bad ? '_' : c;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    size_t start = 0;
    while (start < out.size() && out[start] == ' ') ++start;
    out.erase(0, start);
    return out.empty() ? std::string("_") : out;
}

// "<base> - <output>.mp4".
inline std::string exportFileName(const std::string& base, const std::string& outputName) {
    const std::string b = safeFileNamePart(base.empty() ? std::string("Export") : base);
    return b + " - " + safeFileNamePart(outputName) + ".mp4";
}

// exportFileName for each output, with " (2)", " (3)"... where two would be the same
// file: outputs may share a name, and "Left:" and "Left?" both become "Left_".
// Compared ignoring ASCII case, as Windows does.
inline std::vector<std::string> exportFileNames(const std::string& base,
                                                const std::vector<std::string>& outputNames) {
    auto folded = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    };
    std::vector<std::string> names;
    std::unordered_set<std::string> taken;
    for (const std::string& output : outputNames) {
        const std::string name = exportFileName(base, output);
        std::string candidate = name;
        for (int n = 2; taken.count(folded(candidate)); ++n) {
            candidate = name.substr(0, name.size() - 4) + " (" + std::to_string(n) + ").mp4";
        }
        taken.insert(folded(candidate));
        names.push_back(candidate);
    }
    return names;
}

}  // namespace quadmapper
