#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

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

    bool create(const char* vertexSource, const char* fragmentSource);

    void destroy();

    void bind() const;
    static void unbind();

    // Uniform setters. Each remembers the value it last gave the program and skips
    // the GL call when asked for the same value again (ADR-054: most of a quad's
    // uniforms repeat from the previous draw). Only these setters may change this
    // program's uniforms, or the remembered values go stale.
    void setInt(const char* name, int value) const;
    void setFloat(const char* name, float value) const;
    void setVec2(const char* name, const glm::vec2& value) const;
    void setVec3(const char* name, const glm::vec3& value) const;
    void setVec4(const char* name, const glm::vec4& value) const;
    void setMat3(const char* name, const glm::mat3& value) const;
    void setMat4(const char* name, const glm::mat4& value) const;

    // Query
    bool isValid() const { return programId_ != 0; }
    uint32_t getId() const { return programId_; }

private:
    uint32_t programId_ = 0;

    // A uniform's location, looked up from GL once per name and program, and the
    // bytes of the value last set through it (up to a mat4).
    struct Uniform {
        std::string name;
        int location = -1;
        size_t size = 0;  // bytes in value; 0 = nothing set yet
        unsigned char value[sizeof(float) * 16] = {};
    };

    // Uniforms in the order first asked for, and their indices by name. The keys
    // view Uniform::name inside uniforms_ (a deque, so elements never move).
    mutable std::deque<Uniform> uniforms_;
    mutable std::unordered_map<std::string_view, size_t> uniformsByName_;
    mutable size_t lastUniform_ = static_cast<size_t>(-1);  // index of the last lookup

    Uniform& uniform(const char* name) const;
    // True (and remembered) when `size` bytes at `data` differ from the uniform's
    // current value, i.e. when GL must be told; false for a missing uniform.
    static bool changes(Uniform& u, const void* data, size_t size);
    static uint32_t compileShader(uint32_t type, const char* source);
    static bool checkShaderCompileError(uint32_t shader, const char* type);
    static bool checkProgramLinkError(uint32_t program);
};

} // namespace quadmapper
