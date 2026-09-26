#include "ShaderProgram.hpp"
#include "core/GlObjectStats.hpp"
#include "core/Logger.hpp"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>

namespace quadmapper {

ShaderProgram::ShaderProgram() = default;

ShaderProgram::~ShaderProgram() {
    destroy();
}

// Moving a deque keeps its elements in place, so the moved map's keys and values
// stay valid.
ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept
    : programId_(other.programId_)
    , uniforms_(std::move(other.uniforms_))
    , uniformsByName_(std::move(other.uniformsByName_)) {
    other.programId_ = 0;
    other.uniformsByName_.clear();
    other.uniforms_.clear();
}

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
    if (this != &other) {
        destroy();
        programId_ = other.programId_;
        uniforms_ = std::move(other.uniforms_);
        uniformsByName_ = std::move(other.uniformsByName_);
        other.programId_ = 0;
        other.uniformsByName_.clear();
        other.uniforms_.clear();
    }
    return *this;
}

bool ShaderProgram::create(const char* vertexSource, const char* fragmentSource) {
    uniformsByName_.clear();  // locations and values belong to the program being replaced
    uniforms_.clear();

    // Compile shaders
    uint32_t vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
    if (vertexShader == 0) {
        return false;
    }

    uint32_t fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (fragmentShader == 0) {
        glDeleteShader(vertexShader);
        return false;
    }

    // Create and link program
    programId_ = glCreateProgram();
    GlObjectStats::onShaderProgramCreated();
    glAttachShader(programId_, vertexShader);
    glAttachShader(programId_, fragmentShader);
    glLinkProgram(programId_);

    // Check for link errors
    if (!checkProgramLinkError(programId_)) {
        glDeleteProgram(programId_);
        GlObjectStats::onShaderProgramDestroyed();
        programId_ = 0;
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }

    // Clean up shaders (they're linked into the program now)
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    Logger::debug("Shader program created successfully (ID: {})", programId_);
    return true;
}

void ShaderProgram::destroy() {
    uniformsByName_.clear();
    uniforms_.clear();
    if (programId_ != 0) {
        glDeleteProgram(programId_);
        GlObjectStats::onShaderProgramDestroyed();
        programId_ = 0;
    }
}

void ShaderProgram::bind() const {
    glUseProgram(programId_);
}

void ShaderProgram::unbind() {
    glUseProgram(0);
}

ShaderProgram::Uniform& ShaderProgram::uniform(const char* name) const {
    // A program's uniform locations are fixed once it is linked, and every draw sets
    // a couple of dozen of them, so ask GL only the first time. Draws set them in
    // the same order each time, so the uniform after the last one asked for is
    // tried before the map (one string compare instead of a hash lookup).
    const size_t next = lastUniform_ + 1;
    if (next < uniforms_.size() && uniforms_[next].name == name) {
        lastUniform_ = next;
        return uniforms_[next];
    }
    const auto found = uniformsByName_.find(std::string_view(name));
    if (found != uniformsByName_.end()) {
        lastUniform_ = found->second;
        return uniforms_[found->second];
    }
    Uniform& u = uniforms_.emplace_back();
    u.name = name;
    u.location = glGetUniformLocation(programId_, name);
    lastUniform_ = uniforms_.size() - 1;
    uniformsByName_.emplace(u.name, lastUniform_);
    return u;
}

bool ShaderProgram::changes(Uniform& u, const void* data, size_t size) {
    if (u.location < 0) {
        return false;
    }
    if (u.size == size && std::memcmp(u.value, data, size) == 0) {
        return false;
    }
    std::memcpy(u.value, data, size);
    u.size = size;
    return true;
}

void ShaderProgram::setInt(const char* name, int value) const {
    Uniform& u = uniform(name);
    if (changes(u, &value, sizeof(value))) glUniform1i(u.location, value);
}

void ShaderProgram::setFloat(const char* name, float value) const {
    Uniform& u = uniform(name);
    if (changes(u, &value, sizeof(value))) glUniform1f(u.location, value);
}

void ShaderProgram::setVec2(const char* name, const glm::vec2& value) const {
    Uniform& u = uniform(name);
    if (changes(u, glm::value_ptr(value), sizeof(value))) {
        glUniform2fv(u.location, 1, glm::value_ptr(value));
    }
}

void ShaderProgram::setVec3(const char* name, const glm::vec3& value) const {
    Uniform& u = uniform(name);
    if (changes(u, glm::value_ptr(value), sizeof(value))) {
        glUniform3fv(u.location, 1, glm::value_ptr(value));
    }
}

void ShaderProgram::setVec4(const char* name, const glm::vec4& value) const {
    Uniform& u = uniform(name);
    if (changes(u, glm::value_ptr(value), sizeof(value))) {
        glUniform4fv(u.location, 1, glm::value_ptr(value));
    }
}

void ShaderProgram::setMat4(const char* name, const glm::mat4& value) const {
    Uniform& u = uniform(name);
    if (changes(u, glm::value_ptr(value), sizeof(value))) {
        glUniformMatrix4fv(u.location, 1, GL_FALSE, glm::value_ptr(value));
    }
}

uint32_t ShaderProgram::compileShader(uint32_t type, const char* source) {
    uint32_t shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    const char* typeName = (type == GL_VERTEX_SHADER) ? "vertex" : "fragment";
    if (!checkShaderCompileError(shader, typeName)) {
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool ShaderProgram::checkShaderCompileError(uint32_t shader, const char* type) {
    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if (!success) {
        char infoLog[1024];
        glGetShaderInfoLog(shader, 1024, nullptr, infoLog);
        Logger::error("Shader compilation failed ({}): {}", type, infoLog);
        return false;
    }

    return true;
}

bool ShaderProgram::checkProgramLinkError(uint32_t program) {
    int success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if (!success) {
        char infoLog[1024];
        glGetProgramInfoLog(program, 1024, nullptr, infoLog);
        Logger::error("Shader program link failed: {}", infoLog);
        return false;
    }

    return true;
}

} // namespace quadmapper
