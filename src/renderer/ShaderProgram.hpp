#pragma once

#include <string>
#include <cstdint>
#include <glm/glm.hpp>

namespace quadmapper {

class ShaderProgram {
public:
    ShaderProgram();
    ~ShaderProgram();

    // Non-copyable
    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;

    // Move constructor/assignment
    ShaderProgram(ShaderProgram&& other) noexcept;
    ShaderProgram& operator=(ShaderProgram&& other) noexcept;

    // Create from source strings
    bool create(const char* vertexSource, const char* fragmentSource);

    // Cleanup
    void destroy();

    // Bind/unbind
    void bind() const;
    static void unbind();

    // Uniform setters
    void setInt(const char* name, int value) const;
    void setFloat(const char* name, float value) const;
    void setVec2(const char* name, const glm::vec2& value) const;
    void setVec3(const char* name, const glm::vec3& value) const;
    void setVec4(const char* name, const glm::vec4& value) const;
    void setMat4(const char* name, const glm::mat4& value) const;

    // Query
    bool isValid() const { return programId_ != 0; }
    uint32_t getId() const { return programId_; }

private:
    uint32_t programId_ = 0;

    int getUniformLocation(const char* name) const;
    static uint32_t compileShader(uint32_t type, const char* source);
    static bool checkShaderCompileError(uint32_t shader, const char* type);
    static bool checkProgramLinkError(uint32_t program);
};

} // namespace quadmapper
