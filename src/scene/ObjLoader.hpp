#pragma once

// Wavefront OBJ import for 3D objects (ADR-052 §2). Geometry only: materials,
// objects, groups and smoothing groups are ignored.

#include "MeshObject.hpp"  // MeshData
#include "core/Result.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace quadmapper {

// Reads v / vt / vn and f (v, v/vt, v//vn or v/vt/vn, negative indices allowed),
// fan-triangulates polygons and de-duplicates vertices by their (v, vt, vn) triple.
// Without normals in the file, smooth normals are computed; without uvs, uvs stay
// empty. Malformed, empty or oversized files return Err with the offending line.
// `cancel`, when given, is polled while parsing; once it is set the parse stops and
// returns Err.
Result<MeshData, Error> loadObj(const std::filesystem::path& path,
                                const std::atomic<bool>* cancel = nullptr);

// Parses OBJ files on one worker thread (TD-16a), so importing or opening a show with
// a large mesh never stalls the render thread or the outputs. Requests are parsed in
// order; the main thread takes the results (takeCompleted) and adopts them at its
// frame boundary. Thread-safe; the worker starts with the first request.
class ObjLoadQueue {
public:
    struct Completed {
        uint64_t ticket = 0;
        std::filesystem::path path;
        Result<MeshData, Error> mesh;
    };

    ObjLoadQueue() = default;
    ~ObjLoadQueue();  // stops a parse in progress and joins the worker
    ObjLoadQueue(const ObjLoadQueue&) = delete;
    ObjLoadQueue& operator=(const ObjLoadQueue&) = delete;

    // Queue a parse; the returned ticket (never 0) identifies its result.
    uint64_t request(const std::filesystem::path& path);
    // Drop one request: queued, being parsed (the parse stops early) or finished but
    // not taken. Its result never arrives.
    void cancel(uint64_t ticket);
    // cancel() for every request (New, Load, shutdown).
    void cancelAll();
    // Results finished since the last call, in completion order.
    std::vector<Completed> takeCompleted();
    // Requests queued or being parsed (finished results not yet taken excluded).
    size_t pendingCount() const;

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::pair<uint64_t, std::filesystem::path>> queue_;
    uint64_t running_ = 0;  // ticket being parsed, 0 = none
    std::atomic<bool> cancelRunning_{false};
    std::vector<Completed> done_;
    uint64_t nextTicket_ = 1;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace quadmapper
