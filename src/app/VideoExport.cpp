#include "VideoExport.hpp"

#include "renderer/Framebuffer.hpp"  // glad, before anything that pulls in GLFW
#include "renderer/Renderer.hpp"
#include "audio/OfflineAudioMix.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "editor/EditorState.hpp"
#include "editor/MeshLoads.hpp"
#include "editor/MissingMedia.hpp"
#include "editor/ProjectMapping.hpp"
#include "editor/SourceAudio.hpp"
#include "editor/TextureResolve.hpp"
#include "output/OutputManager.hpp"
#include "project/Project.hpp"
#include "source/SourceManager.hpp"
#include "source/TextureSource.hpp"
#include "video/Mp4Writer.hpp"

#include <algorithm>
#include <thread>

namespace quadmapper {

namespace {

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 2;
// How long one app frame may spend rendering export frames: the window stays live.
constexpr std::chrono::milliseconds FRAME_BUDGET(40);
// Media that has not opened by then is drawn as missing (black).
constexpr std::chrono::seconds OPEN_TIMEOUT(30);
// A video that has not decoded its frame by then is drawn with the frame it has.
constexpr std::chrono::seconds DECODE_TIMEOUT(5);
// After a few frames like that, a source that keeps missing is not waited for long.
constexpr std::chrono::milliseconds LATE_DECODE_TIMEOUT(50);
constexpr int LATE_FRAMES_BEFORE_SHORT_WAIT = 3;

}  // namespace

VideoExport::VideoExport(VideoExportRequest request, float masterGain)
    : request_(std::move(request)), masterGain_(masterGain),
      clockTicks_(std::make_shared<std::atomic<int64_t>>(0)) {}

VideoExport::~VideoExport() { release(); }

void VideoExport::setClock(double seconds) {
    const auto at = base_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(seconds));
    clockTicks_->store(at.time_since_epoch().count());
}

bool VideoExport::start(EditorState& live, OutputManager& outputs, SourceManager& liveSources,
                        std::string& error) {
    if (request_.outputIds.empty()) {
        error = "Choose at least one output.";
        return false;
    }
    if (request_.fps <= 0 || !(request_.seconds > 0.0)) {
        error = "The length must be more than 0 seconds.";
        return false;
    }
    std::error_code ec;
    if (request_.folder.empty() || !std::filesystem::is_directory(request_.folder, ec)) {
        error = "Choose a folder to save the files in.";
        return false;
    }
    for (const UUID& id : request_.outputIds) {
        if (!outputs.getOutput(id)) {
            error = "An output to export no longer exists.";
            return false;
        }
    }

    // The show as it would be saved, loaded into a private editor state and sources
    // that read the export clock. Scene, quad and source ids are kept.
    Project project;
    syncEditorToProject(live, &outputs, &liveSources, project);
    base_ = std::chrono::steady_clock::now();
    setClock(0.0);
    es_ = std::make_unique<EditorState>();
    sources_ = std::make_unique<SourceManager>();
    sources_->prepareGpu();
    std::shared_ptr<std::atomic<int64_t>> ticks = clockTicks_;
    sources_->setTimeSource([ticks] {
        return PlaybackTimePoint(std::chrono::steady_clock::duration(ticks->load()));
    });
    syncProjectToEditor(*es_, nullptr, project, sources_.get(), nullptr);

    frameCount_ = exportFrameCount(request_.seconds, request_.fps);
    nextFrame_ = 0;
    phase_ = Phase::Opening;
    openingSince_ = std::chrono::steady_clock::now();
    status_ = VideoExportStatus{};
    status_.active = true;
    status_.message = "Opening the media...";
    Logger::info("Video export: " + std::to_string(request_.outputIds.size()) + " output(s), " +
                 std::to_string(frameCount_) + " frames at " + std::to_string(request_.fps) + " fps");
    return true;
}

void VideoExport::step(EditorState& live, OutputManager& outputs, Renderer& renderer) {
    if (!status_.active) return;
    if (cancelRequested_) {
        release();
        status_.active = false;
        status_.finished = true;
        status_.cancelled = true;
        status_.message = "Cancelled. No files were written.";
        Logger::info("Video export cancelled");
        return;
    }
    if (phase_ == Phase::Opening) {
        stepOpening(outputs);
        return;
    }
    if (phase_ == Phase::Finishing) {
        finish();  // a frame after the last one, so "Finishing the files..." is shown
        return;
    }
    if (phase_ != Phase::Rendering) return;

    const auto until = std::chrono::steady_clock::now() + FRAME_BUDGET;
    std::string error;
    while (nextFrame_ < frameCount_) {
        if (!framesDue(until)) break;  // a video is late: the editor draws a frame first
        if (!renderFrame(live, outputs, renderer, error)) {
            fail(error);
            return;
        }
        ++nextFrame_;
        if (std::chrono::steady_clock::now() >= until) break;
    }
    status_.progress = frameCount_ > 0 ? static_cast<float>(nextFrame_) / frameCount_ : 1.0f;
    status_.message = "Frame " + std::to_string(nextFrame_) + " of " + std::to_string(frameCount_);
    if (nextFrame_ >= frameCount_) {
        phase_ = Phase::Finishing;
        status_.message = "Finishing the files...";
    }
}

void VideoExport::stepOpening(OutputManager& outputs) {
    sources_->updateAll(0.0);
    adoptFailedOpens(*es_, *sources_);  // unopenable files draw as missing (black)
    adoptLoadedMeshes(*es_);            // the 3D objects' meshes, parsed off the main thread
    bool ready = es_->meshLoads.tickets.empty();
    for (const UUID& id : sources_->getAllSourceIds()) {
        const TextureSource* source = sources_->getSource(id);
        if (source && !source->isReady()) ready = false;
    }
    if (!ready && std::chrono::steady_clock::now() - openingSince_ < OPEN_TIMEOUT) return;
    if (!ready) Logger::warning("Video export: some media or meshes did not open in time; they are missing");

    std::string error;
    if (!openTargets(outputs, error)) {
        fail(error);
        return;
    }
    // The show from its start, playing: every video, also one that was paused, stopped
    // or had finished when the show was saved (a locked one follows the master).
    sources_->seekAllToStart();
    sources_->playMasterTimeline();
    for (const UUID& id : sources_->getAllSourceIds()) {
        PlayableSource* video = sources_->getSourceAs<PlayableSource>(id);
        if (video && !video->isTimelineLocked()) video->play();
    }
    phase_ = Phase::Rendering;
    status_.message = "Rendering...";
}

bool VideoExport::openTargets(OutputManager& outputs, std::string& error) {
    // One file name per output, never two the same (Windows ignores case).
    std::vector<std::string> outputNames;
    for (const UUID& id : request_.outputIds) {
        const OutputWindow* output = outputs.getOutput(id);
        outputNames.push_back(output ? output->getName() : std::string());
    }
    const std::vector<std::string> fileNames = exportFileNames(request_.baseName, outputNames);
    for (const UUID& id : request_.outputIds) {
        const OutputWindow* output = outputs.getOutput(id);
        if (!output) {
            error = "An output to export no longer exists.";
            return false;
        }
        Target target;
        target.outputId = id;
        const IVec2 size = OutputManager::exportFrameSize(*output);
        target.width = size.x;
        target.height = size.y;
        target.path = request_.folder / pathFromUtf8(fileNames[targets_.size()]);

        // The sound of the videos in the scene this output shows, as the live show
        // plays it: each at its volume (0 when muted) times the master volume.
        if (request_.audio && !output->getSceneId().isNull()) {
            auto mix = std::make_unique<OfflineAudioMix>(SAMPLE_RATE, CHANNELS);
            for (const UUID& mediaId : mediaUsedByScenes(es_->scenes, {output->getSceneId()})) {
                TextureSource* source = sources_->getSource(mediaId);
                auto* playable = dynamic_cast<PlayableSource*>(source);
                if (!playable || !source->isReady()) continue;
                const std::filesystem::path audioPath = audioPathOf(*source);
                if (audioPath.empty() || !(playable->getDuration() > 0.0)) continue;
                OfflineAudioClip clip;
                clip.path = audioPath;
                clip.duration = playable->getDuration();
                clip.loopMode = playable->getLoopMode();
                clip.speed = playable->getSpeed();
                clip.gain = (playable->isAudioMuted() ? 0.0f : playable->getAudioVolume()) * masterGain_;
                clip.startOffset = 0.0;  // every video starts with the export
                std::string clipError;
                if (!mix->addClip(clip, clipError)) {
                    Logger::info("Video export: no sound from " + utf8FromPath(audioPath.filename()) + ": " +
                                 clipError);
                }
            }
            if (mix->clipCount() > 0) target.mix = std::move(mix);
        }

        Mp4WriterOptions options;
        options.width = target.width;
        options.height = target.height;
        options.fps = request_.fps;
        options.audio = target.mix != nullptr;
        options.sampleRate = SAMPLE_RATE;
        options.channels = CHANNELS;
        target.writer = std::make_unique<AsyncMp4Writer>();
        std::string openError;
        if (!target.writer->open(target.path, options, openError)) {
            error = "Could not write " + utf8FromPath(target.path.filename()) + ": " + openError;
            target.writer->abort();
            return false;
        }
        Logger::info("Video export: " + utf8FromPath(target.path) + " " + std::to_string(target.width) + "x" +
                     std::to_string(target.height) + " with " + target.writer->encoderName() +
                     (target.mix ? ", with sound" : ", no sound"));
        targets_.push_back(std::move(target));
    }
    return true;
}

bool VideoExport::framesDue(std::chrono::steady_clock::time_point until) {
    using Clock = std::chrono::steady_clock;
    if (!waitingSince_) {
        // The first try for this frame: the clock moves to its time and every video
        // takes the frame due then.
        const double step = 1.0 / request_.fps;
        setClock(nextFrame_ * step);
        sources_->updateAll(nextFrame_ == 0 ? 0.0 : step);
        waitingSince_ = Clock::now();
    } else {
        sources_->updateAll(0.0);
    }
    const Clock::duration timeout = lateFrames_ >= LATE_FRAMES_BEFORE_SHORT_WAIT
                                        ? std::chrono::duration_cast<Clock::duration>(LATE_DECODE_TIMEOUT)
                                        : std::chrono::duration_cast<Clock::duration>(DECODE_TIMEOUT);
    while (!sources_->framesCoverClock()) {
        const Clock::time_point now = Clock::now();
        if (now - *waitingSince_ >= timeout) {
            if (lateFrames_++ < LATE_FRAMES_BEFORE_SHORT_WAIT) {
                Logger::warning("Video export: a video was late at frame " + std::to_string(nextFrame_));
            }
            break;  // drawn with the frame it has
        }
        if (now >= until) return false;  // the wait goes on in the next app frame
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        sources_->updateAll(0.0);
    }
    waitingSince_.reset();
    return true;
}

bool VideoExport::renderFrame(EditorState& live, OutputManager& outputs, Renderer& renderer,
                              std::string& error) {
    const int n = nextFrame_;
    adoptFailedOpens(*es_, *sources_);

    // The snapshot's textures, plus the live test grids of quads without media (they
    // are not part of the saved project).
    auto textures = buildQuadTextureMap(*es_, sources_.get(), SurfaceView::Output);
    for (const auto& scene : es_->scenes) {
        if (!scene) continue;
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            if (textures.count(quad->getId())) continue;
            auto grid = live.quadTextures.find(quad->getId());
            if (grid != live.quadTextures.end() && grid->second) textures[quad->getId()] = grid->second.get();
        }
    }

    outputs.setExportRendering(true);
    outputs.renderOutputFrames(es_->scenes, textures, renderer);
    outputs.setExportRendering(false);

    GLint previousDraw = 0, previousRead = 0, previousPack = 4;
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousPack);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    bool ok = true;
    for (Target& target : targets_) {
        if (!outputs.getOutput(target.outputId)) {
            error = "An output being exported was removed.";
            ok = false;
            break;
        }
        const Framebuffer* frame = outputs.outputFrame(target.outputId);
        if (!frame) {
            error = "Could not draw an output's frame (out of video memory?).";
            ok = false;
            break;
        }
        if (frame->width() != target.width || frame->height() != target.height) {
            error = "An output's resolution changed during the export.";
            ok = false;
            break;
        }
        pixels_.resize(static_cast<size_t>(target.width) * target.height * 4);
        frame->bind();
        glReadPixels(0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
        if (!target.writer->writeVideoFrame(pixels_.data(), target.width * 4, true)) {
            error = "Could not write " + utf8FromPath(target.path.filename()) + ": " + target.writer->error();
            ok = false;
            break;
        }
        if (target.mix) {
            const SampleRange range = audioSamplesForFrame(n, request_.fps, SAMPLE_RATE);
            samples_.resize(static_cast<size_t>(range.count) * CHANNELS);
            target.mix->render(static_cast<double>(range.first) / SAMPLE_RATE, static_cast<size_t>(range.count),
                               samples_.data());
            if (!target.writer->writeAudio(samples_.data(), static_cast<int>(range.count))) {
                error = "Could not write " + utf8FromPath(target.path.filename()) + ": " + target.writer->error();
                ok = false;
                break;
            }
        }
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
    glPixelStorei(GL_PACK_ALIGNMENT, previousPack);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    return ok;
}

void VideoExport::finish() {
    status_.message = "Finishing the files...";
    std::vector<std::filesystem::path> written;
    for (Target& target : targets_) {
        std::filesystem::path path;
        std::string error;
        if (!target.writer->finish(path, error)) {
            fail("Could not finish " + utf8FromPath(target.path.filename()) + ": " + error);
            status_.written = std::move(written);  // the files finished before it are kept
            return;
        }
        written.push_back(path);
    }
    release();
    status_.active = false;
    status_.finished = true;
    status_.progress = 1.0f;
    status_.written = std::move(written);
    status_.message = "Done.";
    Logger::info("Video export done: " + std::to_string(status_.written.size()) + " file(s)");
}

void VideoExport::fail(const std::string& message) {
    Logger::error("Video export failed: " + message);
    release();
    status_.active = false;
    status_.finished = true;
    status_.failed = true;
    status_.message = message;
}

void VideoExport::release() {
    for (Target& target : targets_) {
        if (target.writer) target.writer->abort();  // a finished writer ignores it
    }
    targets_.clear();
    sources_.reset();
    es_.reset();
    phase_ = Phase::Idle;
}

}  // namespace quadmapper
