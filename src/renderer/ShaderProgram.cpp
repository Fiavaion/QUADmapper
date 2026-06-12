#include "ShaderProgram.hpp"
#include "core/Logger.hpp"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

namespace quadmapper {

ShaderProgram::ShaderProgram() = default;

ShaderProgram::~ShaderProgram() {
    destroy();
}

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept
    : programId_(other.programId_) {
    other.programId_ = 0;
}

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
    if (this != &other) {
        destroy();
        programId_ = other.programId_;
        other.programId_ = 0;
    }
    return *this;
}

bool ShaderProgram::create(const char* vertexSource, const char* fragmentSource) {
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
    glAttachShader(programId_, vertexShader);
    glAttachShader(programId_, fragmentShader);
    glLinkProgram(programId_);

    // Check for link errors
    if (!checkProgramLinkError(programId_)) {
        glDeleteProgram(programId_);
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
    if (programId_ != 0) {
        glDeleteProgram(programId_);
        programId_ = 0;
    }
}

void ShaderProgram::bind() const {
    glUseProgram(programId_);
}

void ShaderProgram::unbind() {
    glUseProgram(0);
}

int ShaderProgram::getUniformLocation(const char* name) const {
    return glGetUniformLocation(programId_, name);
}

void ShaderProgram::setInt(const char* name, int value) const {
    glUniform1i(getUniformLocation(name), value);
}

void ShaderProgram::setFloat(const char* name, float value) const {
    glUniform1f(getUniformLocation(name), value);
}

void ShaderProgram::setVec2(const char* name, const glm::vec2& value) const {
    glUniform2fv(getUniformLocation(name), 1, glm::value_ptr(value));
}

void ShaderProgram::setVec3(const char* name, const glm::vec3& value) const {
    glUniform3fv(getUniformLocation(name), 1, glm::value_ptr(value));
}

void ShaderProgram::setVec4(const char* name, const glm::vec4& value) const {
    glUniform4fv(getUniformLocation(name), 1, glm::value_ptr(value));
}

void ShaderProgram::setMat4(const char* name, const glm::mat4& value) const {
    glUniformMatrix4fv(getUniformLocation(name), 1, GL_FALSE, glm::value_ptr(value));
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
