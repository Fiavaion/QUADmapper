#include "ObjLoader.hpp"
#include "core/FileSystem.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace quadmapper {

namespace {

namespace fs = std::filesystem;

// Guards against files that would exhaust memory or overflow 32-bit indices.
constexpr std::uintmax_t kMaxFileBytes = 512ull * 1024 * 1024;
constexpr size_t kMaxElements = size_t(1) << 26;  // per v/vt/vn list, and per triangle list

// One face corner as resolved 0-based indices; -1 = not given.
struct Corner {
    int v = -1;
    int vt = -1;
    int vn = -1;
    bool operator==(const Corner& o) const { return v == o.v && vt == o.vt && vn == o.vn; }
};

struct CornerHash {
    size_t operator()(const Corner& c) const {
        size_t h = std::hash<int>()(c.v);
        h = h * 1000003u ^ std::hash<int>()(c.vt);
        h = h * 1000003u ^ std::hash<int>()(c.vn);
        return h;
    }
};

void splitWhitespace(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
}

bool parseFloat(std::string_view s, float& out) {
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size() && std::isfinite(out);
}

bool parseInt(std::string_view s, int& out) {
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size();
}

// OBJ indices are 1-based; negative ones count back from the latest element.
bool resolveIndex(std::string_view s, size_t count, int& out) {
    int i = 0;
    if (!parseInt(s, i) || i == 0) return false;
    const long long resolved = i > 0 ? static_cast<long long>(i) - 1
                                     : static_cast<long long>(count) + i;
    if (resolved < 0 || resolved >= static_cast<long long>(count)) return false;
    out = static_cast<int>(resolved);
    return true;
}

}  // namespace

Result<MeshData, Error> loadObj(const fs::path& path, const std::atomic<bool>* cancel) {
    const std::string name = utf8FromPath(path.filename());
    std::error_code sizeError;
    const std::uintmax_t bytes = fs::file_size(path, sizeError);
    if (sizeError) return Err<MeshData>("Cannot open OBJ file: " + utf8FromPath(path));
    if (bytes > kMaxFileBytes) {
        return Err<MeshData>(name + ": file is too large to import (over 512 MB)");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return Err<MeshData>("Cannot open OBJ file: " + utf8FromPath(path));

    std::vector<Vec3> positions;
    std::vector<Vec2> texcoords;
    std::vector<Vec3> normals;

    MeshData mesh;
    std::vector<int> positionOf;  // output vertex -> OBJ position index (for smooth normals)
    std::unordered_map<Corner, uint32_t, CornerHash> vertexOf;
    bool anyUv = false;
    bool allNormals = true;

    std::string line;
    std::vector<std::string_view> tokens;
    std::vector<uint32_t> face;
    size_t lineNo = 0;
    auto fail = [&](const std::string& what) {
        return Err<MeshData>(name + " line " + std::to_string(lineNo) + ": " + what);
    };

    while (std::getline(in, line)) {
        ++lineNo;
        if (cancel && (lineNo & 0xFFF) == 0 && cancel->load(std::memory_order_relaxed)) {
            return Err<MeshData>(name + ": loading cancelled");
        }
        std::string_view view(line);
        if (const size_t hash = view.find('#'); hash != std::string_view::npos) {
            view = view.substr(0, hash);
        }
        splitWhitespace(view, tokens);
        if (tokens.empty()) continue;
        const std::string_view kw = tokens[0];

        if (kw == "v" || kw == "vn") {
            // Extra values (a w component, vertex colours) are ignored.
            Vec3 p;
            if (tokens.size() < 4 || !parseFloat(tokens[1], p.x) || !parseFloat(tokens[2], p.y) ||
                !parseFloat(tokens[3], p.z)) {
                return fail("expected three numbers after '" + std::string(kw) + "'");
            }
            auto& list = (kw == "v") ? positions : normals;
            if (list.size() >= kMaxElements) return fail("too many elements");
            list.push_back(p);
        } else if (kw == "vt") {
            Vec2 t(0.0f);
            if (tokens.size() < 2 || !parseFloat(tokens[1], t.x) ||
                (tokens.size() > 2 && !parseFloat(tokens[2], t.y))) {
                return fail("expected numbers after 'vt'");
            }
            if (texcoords.size() >= kMaxElements) return fail("too many elements");
            // OBJ puts v = 0 at the image bottom; frames are stored top row first
            // (v = 0 = image top), so v is flipped here.
            texcoords.push_back(Vec2(t.x, 1.0f - t.y));
        } else if (kw == "f") {
            if (tokens.size() < 4) return fail("a face needs at least three vertices");
            face.clear();
            for (size_t k = 1; k < tokens.size(); ++k) {
                const std::string_view token = tokens[k];
                std::string_view parts[3];
                size_t partCount = 0;
                size_t start = 0;
                while (true) {
                    const size_t slash = token.find('/', start);
                    if (partCount == 3) return fail("bad face vertex '" + std::string(token) + "'");
                    parts[partCount++] = token.substr(start, slash == std::string_view::npos
                                                                 ? std::string_view::npos
                                                                 : slash - start);
                    if (slash == std::string_view::npos) break;
                    start = slash + 1;
                }
                Corner c;
                if (!resolveIndex(parts[0], positions.size(), c.v) ||
                    (partCount > 1 && !parts[1].empty() &&
                     !resolveIndex(parts[1], texcoords.size(), c.vt)) ||
                    (partCount > 2 && !parts[2].empty() &&
                     !resolveIndex(parts[2], normals.size(), c.vn))) {
                    return fail("bad index '" + std::string(token) + "'");
                }

                auto it = vertexOf.find(c);
                if (it == vertexOf.end()) {
                    const auto index = static_cast<uint32_t>(mesh.positions.size());
                    mesh.positions.push_back(positions[c.v]);
                    mesh.uvs.push_back(c.vt >= 0 ? texcoords[c.vt] : Vec2(0.0f));
                    mesh.normals.push_back(c.vn >= 0 ? normals[c.vn] : Vec3(0.0f));
                    positionOf.push_back(c.v);
                    anyUv = anyUv || c.vt >= 0;
                    allNormals = allNormals && c.vn >= 0;
                    it = vertexOf.emplace(c, index).first;
                }
                face.push_back(it->second);
            }
            if (mesh.indices.size() / 3 + face.size() > kMaxElements) {
                return fail("too many faces");
            }
            for (size_t k = 1; k + 1 < face.size(); ++k) {  // fan
                mesh.indices.insert(mesh.indices.end(), {face[0], face[k], face[k + 1]});
            }
        }
        // Anything else (o, g, s, usemtl, mtllib, l, p, ...) carries no geometry we use.
    }
    if (in.bad()) return Err<MeshData>(name + ": read error");
    if (mesh.indices.empty()) return Err<MeshData>(name + ": no faces found");

    if (!anyUv) mesh.uvs.clear();
    if (!allNormals) {
        // Smooth normals: area-weighted face normals summed per OBJ position, so
        // vertices split only by their uv still share one normal.
        std::vector<Vec3> sum(positions.size(), Vec3(0.0f));
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const uint32_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
            const Vec3 n = glm::cross(mesh.positions[b] - mesh.positions[a],
                                      mesh.positions[c] - mesh.positions[a]);
            sum[positionOf[a]] += n;
            sum[positionOf[b]] += n;
            sum[positionOf[c]] += n;
        }
        for (size_t i = 0; i < mesh.normals.size(); ++i) {
            const Vec3 n = sum[positionOf[i]];
            const float len = glm::length(n);
            mesh.normals[i] = len > 1e-12f ? n / len : Vec3(0.0f, 0.0f, 1.0f);
        }
    }
    return Ok(std::move(mesh));
}

// ---- ObjLoadQueue ----------------------------------------------------------------

ObjLoadQueue::~ObjLoadQueue() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        queue_.clear();
        cancelRunning_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

uint64_t ObjLoadQueue::request(const fs::path& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t ticket = nextTicket_++;
    queue_.emplace_back(ticket, path);
    if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
    wake_.notify_one();
    return ticket;
}

void ObjLoadQueue::cancel(uint64_t ticket) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->first == ticket) {
            queue_.erase(it);
            return;
        }
    }
    if (running_ == ticket) {
        cancelRunning_ = true;
        return;
    }
    for (auto it = done_.begin(); it != done_.end(); ++it) {
        if (it->ticket == ticket) {
            done_.erase(it);
            return;
        }
    }
}

void ObjLoadQueue::cancelAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    done_.clear();
    if (running_ != 0) cancelRunning_ = true;
}

std::vector<ObjLoadQueue::Completed> ObjLoadQueue::takeCompleted() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Completed> out;
    out.swap(done_);
    return out;
}

size_t ObjLoadQueue::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() + (running_ != 0 ? 1 : 0);
}

void ObjLoadQueue::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_) return;
        auto [ticket, path] = std::move(queue_.front());
        queue_.pop_front();
        running_ = ticket;
        cancelRunning_ = false;
        lock.unlock();

        Result<MeshData, Error> mesh = loadObj(path, &cancelRunning_);

        lock.lock();
        running_ = 0;
        // A request cancelled while it was parsed leaves no result.
        if (!cancelRunning_ && !stop_) {
            done_.push_back(Completed{ticket, std::move(path), std::move(mesh)});
        }
    }
}

}  // namespace quadmapper
