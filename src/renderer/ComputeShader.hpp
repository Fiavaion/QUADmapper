#pragma once

#include "core/Types.hpp"
#include <string>
#include <cstdint>
#include <unordered_map>

namespace quadmapper {

/**
 * @brief Wrapper for OpenGL compute shaders
 *
 * Provides a simple interface for loading, compiling, and dispatching
 * compute shaders. Used by procedural texture sources to generate
 * textures directly on the GPU.
 *
 * Requires OpenGL 4.3+ (compute shader support).
 */
class ComputeShader {
public:
    ComputeShader();
    ~ComputeShader();

    // Non-copyable, movable
    ComputeShader(const ComputeShader&) = delete;
    ComputeShader& operator=(const ComputeShader&) = delete;
    ComputeShader(ComputeShader&& other) noexcept;
    ComputeShader& operator=(ComputeShader&& other) noexcept;

    /**
     * @brief Load and compile a compute shader from source
     * @param source GLSL compute shader source code
     * @return true if compilation succeeded
     */
    bool loadFromSource(const std::string& source);

    /**
     * @brief Check if the shader is valid and ready to use
     */
    bool isValid() const { return programId_ != 0; }

    /**
     * @brief Bind the shader for use
     */
    void bind() const;

    /**
     * @brief Unbind any compute shader
     */
    static void unbind();

    /**
     * @brief Dispatch the compute shader
     * @param groupsX Number of work groups in X dimension
     * @param groupsY Number of work groups in Y dimension
     * @param groupsZ Number of work groups in Z dimension (default 1)
     *
     * The total threads = groups * local_size (defined in shader)
     */
    void dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ = 1) const;

    /**
     * @brief Wait for compute shader to finish
     *
     * Issues a memory barrier to ensure image writes are visible.
     */
    void waitForCompletion() const;

    /**
     * @brief Bind an image texture for reading/writing
     * @param unit Image unit index (0-7 typically)
     * @param texture OpenGL texture ID
     * @param access GL_READ_ONLY, GL_WRITE_ONLY, or GL_READ_WRITE
     * @param format Internal format (e.g., GL_RGBA8)
     */
    void bindImageTexture(uint32_t unit, uint32_t texture, uint32_t access, uint32_t format) const;

    // --- Uniform setters ---
    // Note: Color is an alias for Vec4, use setUniform(name, Vec4) for colors

    void setUniform(const std::string& name, int value);
    void setUniform(const std::string& name, float value);
    void setUniform(const std::string& name, const Vec2& value);
    void setUniform(const std::string& name, const Vec3& value);
    void setUniform(const std::string& name, const Vec4& value);

    /**
     * @brief Get the OpenGL program ID
     */
    uint32_t getProgramId() const { return programId_; }

    /**
     * @brief Get the last compile/link error message
     */
    const std::string& getErrorMessage() const { return errorMessage_; }

private:
    int getUniformLocation(const std::string& name);

    uint32_t programId_ = 0;
    std::string errorMessage_;
    std::unordered_map<std::string, int> uniformLocationCache_;
};

} // namespace quadmapper
