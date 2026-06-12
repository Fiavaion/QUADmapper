#include "ComputeShader.hpp"
#include "core/Logger.hpp"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// OpenGL 4.3 compute shader constants (not in GLAD 3.3)
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#endif
#ifndef GL_TEXTURE_FETCH_BARRIER_BIT
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x00000008
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif

// Function pointer types for OpenGL 4.3 compute shader functions
typedef void (APIENTRY *PFNGLPRIVATEDISPATCHCOMPUTEPROC)(GLuint, GLuint, GLuint);
typedef void (APIENTRY *PFNGLPRIVATEMEMORYBARRIERPROC)(GLbitfield);
typedef void (APIENTRY *PFNGLPRIVATEBINDIMAGETEXTUREPROC)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);

namespace quadmapper {

// Wrapper function to get proc address
static void* getGLProcAddress(const char* name) {
#ifdef _WIN32
    void* p = (void*)wglGetProcAddress(name);
    if (p == nullptr || (p == (void*)0x1) || (p == (void*)0x2) || (p == (void*)0x3) || (p == (void*)-1)) {
        HMODULE module = LoadLibraryA("opengl32.dll");
        p = (void*)GetProcAddress(module, name);
    }
    return p;
#else
    return nullptr;
#endif
}

// Function pointers for OpenGL 4.3 functions (loaded at runtime)
static PFNGLPRIVATEDISPATCHCOMPUTEPROC glDispatchComputePtr = nullptr;
static PFNGLPRIVATEMEMORYBARRIERPROC glMemoryBarrierPtr = nullptr;
static PFNGLPRIVATEBINDIMAGETEXTUREPROC glBindImageTexturePtr = nullptr;
static bool computeFunctionsLoaded = false;

static bool loadComputeFunctions() {
    if (computeFunctionsLoaded) return true;

    glDispatchComputePtr = (PFNGLPRIVATEDISPATCHCOMPUTEPROC)getGLProcAddress("glDispatchCompute");
    glMemoryBarrierPtr = (PFNGLPRIVATEMEMORYBARRIERPROC)getGLProcAddress("glMemoryBarrier");
    glBindImageTexturePtr = (PFNGLPRIVATEBINDIMAGETEXTUREPROC)getGLProcAddress("glBindImageTexture");

    computeFunctionsLoaded = true;

    if (!glDispatchComputePtr || !glMemoryBarrierPtr || !glBindImageTexturePtr) {
        Logger::warning("ComputeShader: Some compute shader functions not available. OpenGL 4.3+ required.");
        return false;
    }
    return true;
}

ComputeShader::ComputeShader() = default;

ComputeShader::~ComputeShader() {
    if (programId_ != 0) {
        glDeleteProgram(programId_);
        programId_ = 0;
    }
}

ComputeShader::ComputeShader(ComputeShader&& other) noexcept
    : programId_(other.programId_)
    , errorMessage_(std::move(other.errorMessage_))
    , uniformLocationCache_(std::move(other.uniformLocationCache_))
{
    other.programId_ = 0;
}

ComputeShader& ComputeShader::operator=(ComputeShader&& other) noexcept {
    if (this != &other) {
        if (programId_ != 0) {
            glDeleteProgram(programId_);
        }
        programId_ = other.programId_;
        errorMessage_ = std::move(other.errorMessage_);
        uniformLocationCache_ = std::move(other.uniformLocationCache_);
        other.programId_ = 0;
    }
    return *this;
}

bool ComputeShader::loadFromSource(const std::string& source) {
    // Load compute shader functions if not already done
    loadComputeFunctions();

    // Clean up existing program
    if (programId_ != 0) {
        glDeleteProgram(programId_);
        programId_ = 0;
    }
    uniformLocationCache_.clear();
    errorMessage_.clear();

    // Create and compile shader
    GLuint shaderId = glCreateShader(GL_COMPUTE_SHADER);
    if (shaderId == 0) {
        errorMessage_ = "Failed to create compute shader - GL_COMPUTE_SHADER may not be supported";
        Logger::error("{}", errorMessage_);
        return false;
    }

    const char* src = source.c_str();
    glShaderSource(shaderId, 1, &src, nullptr);
    glCompileShader(shaderId);

    // Check compilation
    GLint success;
    glGetShaderiv(shaderId, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint logLength;
        glGetShaderiv(shaderId, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(logLength, '\0');
        glGetShaderInfoLog(shaderId, logLength, nullptr, log.data());
        errorMessage_ = "Compute shader compilation failed: " + log;
        Logger::error("{}", errorMessage_);
        glDeleteShader(shaderId);
        return false;
    }

    // Create program and link
    programId_ = glCreateProgram();
    glAttachShader(programId_, shaderId);
    glLinkProgram(programId_);

    // Check linking
    glGetProgramiv(programId_, GL_LINK_STATUS, &success);
    if (!success) {
        GLint logLength;
        glGetProgramiv(programId_, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(logLength, '\0');
        glGetProgramInfoLog(programId_, logLength, nullptr, log.data());
        errorMessage_ = "Compute shader linking failed: " + log;
        Logger::error("{}", errorMessage_);
        glDeleteShader(shaderId);
        glDeleteProgram(programId_);
        programId_ = 0;
        return false;
    }

    // Clean up shader (program retains it)
    glDetachShader(programId_, shaderId);
    glDeleteShader(shaderId);

    Logger::debug("ComputeShader: Compiled and linked successfully");
    return true;
}

void ComputeShader::bind() const {
    if (programId_ != 0) {
        glUseProgram(programId_);
    }
}

void ComputeShader::unbind() {
    glUseProgram(0);
}

void ComputeShader::dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) const {
    if (programId_ != 0 && glDispatchComputePtr) {
        glDispatchComputePtr(groupsX, groupsY, groupsZ);
    }
}

void ComputeShader::waitForCompletion() const {
    // Ensure all image writes are visible
    if (glMemoryBarrierPtr) {
        glMemoryBarrierPtr(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }
}

void ComputeShader::bindImageTexture(uint32_t unit, uint32_t texture, uint32_t access, uint32_t format) const {
    if (glBindImageTexturePtr) {
        glBindImageTexturePtr(unit, texture, 0, GL_FALSE, 0, access, format);
    }
}

int ComputeShader::getUniformLocation(const std::string& name) {
    auto it = uniformLocationCache_.find(name);
    if (it != uniformLocationCache_.end()) {
        return it->second;
    }

    int location = glGetUniformLocation(programId_, name.c_str());
    uniformLocationCache_[name] = location;
    return location;
}

void ComputeShader::setUniform(const std::string& name, int value) {
    int location = getUniformLocation(name);
    if (location >= 0) {
        glUniform1i(location, value);
    }
}

void ComputeShader::setUniform(const std::string& name, float value) {
    int location = getUniformLocation(name);
    if (location >= 0) {
        glUniform1f(location, value);
    }
}

void ComputeShader::setUniform(const std::string& name, const Vec2& value) {
    int location = getUniformLocation(name);
    if (location >= 0) {
        glUniform2fv(location, 1, glm::value_ptr(value));
    }
}

void ComputeShader::setUniform(const std::string& name, const Vec3& value) {
    int location = getUniformLocation(name);
    if (location >= 0) {
        glUniform3fv(location, 1, glm::value_ptr(value));
    }
}

void ComputeShader::setUniform(const std::string& name, const Vec4& value) {
    int location = getUniformLocation(name);
    if (location >= 0) {
        glUniform4fv(location, 1, glm::value_ptr(value));
    }
}

} // namespace quadmapper
