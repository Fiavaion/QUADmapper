#pragma once

#include <atomic>
#include <string>

// ---------------------------------------------------------------------------
// GlObjectStats — live GL-object counters for the perform-mode leak harness.
//
// One std::atomic<long> per GL-object category.  Instrumentation rules:
//   - Increment exactly when a GL name is *generated* (glGen*/glCreate*).
//   - Decrement exactly when it is *deleted* (glDelete*), guarded by handle != 0.
//   - Move semantics must transfer the name and zero the source so the moved-from
//     dtor decrements nothing.  Each wrapper enforces this independently.
//   - Non-owning "borrowed" handles (e.g. Texture::setGLTextureId) must NOT
//     increment the counter.
//
// Consumed by the H2 stress-harness cycle loop; this header is not dead code.
// ---------------------------------------------------------------------------

namespace quadmapper {

class GlObjectStats {
public:
    // --- per-category increment / decrement -----------------------------------

    static void onTextureCreated()       noexcept { s_textures.fetch_add(1, std::memory_order_relaxed); }
    static void onTextureDestroyed()     noexcept { s_textures.fetch_sub(1, std::memory_order_relaxed); }

    static void onFramebufferCreated()   noexcept { s_framebuffers.fetch_add(1, std::memory_order_relaxed); }
    static void onFramebufferDestroyed() noexcept { s_framebuffers.fetch_sub(1, std::memory_order_relaxed); }

    static void onVertexArrayCreated()   noexcept { s_vertexArrays.fetch_add(1, std::memory_order_relaxed); }
    static void onVertexArrayDestroyed() noexcept { s_vertexArrays.fetch_sub(1, std::memory_order_relaxed); }

    static void onVertexBufferCreated()  noexcept { s_vertexBuffers.fetch_add(1, std::memory_order_relaxed); }
    static void onVertexBufferDestroyed()noexcept { s_vertexBuffers.fetch_sub(1, std::memory_order_relaxed); }

    static void onShaderProgramCreated()   noexcept { s_shaderPrograms.fetch_add(1, std::memory_order_relaxed); }
    static void onShaderProgramDestroyed() noexcept { s_shaderPrograms.fetch_sub(1, std::memory_order_relaxed); }

    // --- read accessors -------------------------------------------------------

    static long textures()      noexcept { return s_textures.load(std::memory_order_relaxed); }
    static long framebuffers()  noexcept { return s_framebuffers.load(std::memory_order_relaxed); }
    static long vertexArrays()  noexcept { return s_vertexArrays.load(std::memory_order_relaxed); }
    static long vertexBuffers() noexcept { return s_vertexBuffers.load(std::memory_order_relaxed); }
    static long shaderPrograms()noexcept { return s_shaderPrograms.load(std::memory_order_relaxed); }

    // --- snapshot / diff ------------------------------------------------------

    struct Snapshot {
        long textures;
        long framebuffers;
        long vertexArrays;
        long vertexBuffers;
        long shaderPrograms;

        static Snapshot capture() noexcept {
            return {
                GlObjectStats::textures(),
                GlObjectStats::framebuffers(),
                GlObjectStats::vertexArrays(),
                GlObjectStats::vertexBuffers(),
                GlObjectStats::shaderPrograms()
            };
        }

        // Returns a human-readable per-type diff table.
        // Shows "baseline -> current (delta)" for each category.
        std::string diff(const Snapshot& baseline) const {
            auto row = [](const char* name, long base, long cur) -> std::string {
                long delta = cur - base;
                std::string sign = (delta > 0) ? "+" : "";
                // Hand-rolled to avoid <format> (C++20) and <sstream> overhead.
                std::string s;
                s += "  ";
                s += name;
                s += ": baseline=";
                s += std::to_string(base);
                s += " current=";
                s += std::to_string(cur);
                s += " (";
                s += sign;
                s += std::to_string(delta);
                s += ")\n";
                return s;
            };

            std::string result = "GL object live-count diff:\n";
            result += row("Texture      ", baseline.textures,      textures);
            result += row("Framebuffer  ", baseline.framebuffers,  framebuffers);
            result += row("VertexArray  ", baseline.vertexArrays,  vertexArrays);
            result += row("VertexBuffer ", baseline.vertexBuffers, vertexBuffers);
            result += row("ShaderProgram", baseline.shaderPrograms,shaderPrograms);
            return result;
        }

        bool isFlat(const Snapshot& baseline) const noexcept {
            return textures      == baseline.textures
                && framebuffers  == baseline.framebuffers
                && vertexArrays  == baseline.vertexArrays
                && vertexBuffers == baseline.vertexBuffers
                && shaderPrograms== baseline.shaderPrograms;
        }
    };

private:
    static std::atomic<long> s_textures;
    static std::atomic<long> s_framebuffers;
    static std::atomic<long> s_vertexArrays;
    static std::atomic<long> s_vertexBuffers;
    static std::atomic<long> s_shaderPrograms;
};

} // namespace quadmapper
