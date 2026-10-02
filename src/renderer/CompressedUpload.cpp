#include "CompressedUpload.hpp"
#include "Texture.hpp"
#include "core/GlObjectStats.hpp"
#include "core/Logger.hpp"

#include <glad/glad.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace quadmapper {

// Copies frames into mapped pixel buffers off the main thread. One thread is plenty:
// 16 x 30 fps of 1080p HapQ (2 MB a frame) is ~1 GB/s. Jobs are the sources' Staging
// slots; each stays alive until its copy is done (CompressedUploadState waits first).
class ByteCopier {
public:
    ByteCopier() : thread_([this] { run(); }) {}
    ~ByteCopier() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();  // after the queue drains: no job is left undone
    }
    ByteCopier(const ByteCopier&) = delete;
    ByteCopier& operator=(const ByteCopier&) = delete;

    void submit(CompressedUploadState::Staging& job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job.done = false;
            queue_.push_back(&job);
        }
        wake_.notify_all();
    }

    void wait(const CompressedUploadState::Staging& job) {
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock, [&job] { return job.done; });
    }

    // Never waits: true once no copy writes into `job`'s mapping (a queued copy is
    // dropped, it never starts), false while its copy runs.
    bool drop(CompressedUploadState::Staging& job) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (job.done) return true;
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (*it == &job) {
                queue_.erase(it);
                job.done = true;
                return true;
            }
        }
        return false;
    }

    static std::function<void()>& hook() {
        static std::function<void()> h;
        return h;
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stopping, and nothing left to copy
            CompressedUploadState::Staging* job = queue_.front();
            queue_.pop_front();
            lock.unlock();
            if (hook()) hook()();
            std::memcpy(job->mapped, job->bytes->data(), job->bytes->size());
            lock.lock();
            job->done = true;
            finished_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable finished_;
    std::deque<CompressedUploadState::Staging*> queue_;
    bool stop_ = false;
    std::thread thread_;  // last: starts once the members above exist
};

// --- CompressedUploadState ---

CompressedUploadState::CompressedUploadState() {
    glGenBuffers(PBO_COUNT, pbos_);
    for (int b = 0; b < PBO_COUNT; ++b) GlObjectStats::onVertexBufferCreated();  // counted with the other GL buffers
}

CompressedUploadState::~CompressedUploadState() {
    settle();
    for (uint32_t& buffer : pbos_) {
        if (buffer != 0) {
            glDeleteBuffers(1, &buffer);
            GlObjectStats::onVertexBufferDestroyed();
        }
    }
}

bool CompressedUploadState::idle() {
    return !staging_.mapped || !copier_ || copier_->drop(staging_);
}

void CompressedUploadState::settle() {
    if (staging_.mapped) {
        if (copier_) copier_->wait(staging_);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[staging_.pbo]);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        staging_.mapped = nullptr;
        staging_.mappedBytes = 0;
    }
    staging_.bytes.reset();
}

// --- CompressedUploader ---

void CompressedUploader::setCopyHookForTesting(std::function<void()> hook) {
    ByteCopier::hook() = std::move(hook);
}

// Orphan the next buffer of the ring (the GPU may still read the frame uploaded from it
// before) and map it for the copier to fill.
bool CompressedUploader::mapStaging(CompressedUploadState& state, size_t bytes) {
    CompressedUploadState::Staging& staged = state.staging_;
    staged.pbo = state.nextPbo_;
    state.nextPbo_ = (state.nextPbo_ + 1) % CompressedUploadState::PBO_COUNT;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, state.pbos_[staged.pbo]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_DRAW);
    staged.mapped = static_cast<uint8_t*>(glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(bytes),
                                                           GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);  // mapped buffers stay unbound: GL must not read them
    staged.mappedBytes = staged.mapped ? bytes : 0;
    return staged.mapped != nullptr;
}

void CompressedUploader::stageCopy(const std::shared_ptr<const std::vector<uint8_t>>& bytes,
                                   CompressedUploadState& state) {
    state.staging_.bytes = bytes;
    copier_->submit(state.staging_);
}

bool CompressedUploader::upload(const std::shared_ptr<const std::vector<uint8_t>>& bytes, Texture& target,
                                CompressedUploadState& state) {
    if (!bytes || bytes->empty() || !target.isCompressed() || state.pbos_[0] == 0) return false;
    if (!copier_) copier_ = std::make_shared<ByteCopier>();
    state.copier_ = copier_;
    CompressedUploadState::Staging& staged = state.staging_;
    const size_t size = bytes->size();
    // The frame prefetch() staged: its bytes are (nearly) in the buffer already.
    bool staging = staged.mapped && staged.bytes == bytes;

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!staging) {
            // Another frame than the one staged (first frame, seek, dropped frames), or
            // the buffer lost its contents: copy this one now, on the copier.
            if (staged.mapped && staged.mappedBytes < size) state.settle();  // too small
            if (staged.mapped) {
                copier_->wait(staged);  // the unwanted copy still writes into the mapping
            } else if (!mapStaging(state, size)) {
                break;
            }
            stageCopy(bytes, state);
        }
        copier_->wait(staged);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, state.pbos_[staged.pbo]);
        const bool intact = glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) == GL_TRUE;
        staged.mapped = nullptr;
        staged.mappedBytes = 0;
        if (intact) {
            target.updateCompressed(nullptr, size);  // from offset 0 of the bound buffer
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            staged.bytes.reset();
            return true;
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        staging = false;  // contents lost (a display mode change): copy again
    }
    staged.bytes.reset();
    Logger::warning("CompressedUploader: could not map a pixel buffer; this video frame was not uploaded");
    return false;
}

void CompressedUploader::prefetch(const std::shared_ptr<const std::vector<uint8_t>>& next,
                                  CompressedUploadState& state) {
    if (!copier_ || !next || next->empty() || state.staging_.mapped || state.pbos_[0] == 0) return;
    state.copier_ = copier_;
    if (mapStaging(state, next->size())) stageCopy(next, state);
}

}  // namespace quadmapper
