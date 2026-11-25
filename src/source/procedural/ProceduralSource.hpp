#pragma once

#include "source/TextureSource.hpp"
#include "renderer/ShaderProgram.hpp"
#include "core/Types.hpp"
#include <variant>
#include <vector>
#include <memory>

namespace quadmapper {

/**
 * @brief Parameter value type for procedural sources
 */
using ParamValue = std::variant<float, int, bool, Color>;

/**
 * @brief Describes a single adjustable parameter
 */
struct SourceParameter {
    enum class Type {
        Float,
        Int,
        Bool,
        Color
    };

    std::string name;           // Internal name (uniform name)
    std::string displayName;    // UI display name
    Type type = Type::Float;
    ParamValue value;
    ParamValue defaultValue;
    float minValue = 0.0f;      // For Float/Int
    float maxValue = 1.0f;      // For Float/Int
};

/**
 * @brief Abstract base class for GPU-based procedural texture sources
 *
 * Procedural sources generate textures with an OpenGL 3.3 fragment shader drawn as one
 * fullscreen triangle into a framebuffer whose colour attachment is the output texture.
 * They render directly to a GPU texture (isGPUNative() == true).
 *
 * Subclasses implement:
 * - getShaderSource() - Return the GLSL fragment shader source
 * - getParameters() - Return adjustable parameters
 * - setParameter() - Handle parameter changes
 * - onUpdate() - Called each frame for animated sources
 */
class ProceduralSource : public TextureSource {
public:
    /**
     * @brief Construct a procedural source
     * @param name Display name
     * @param width Texture width
     * @param height Texture height
     */
    ProceduralSource(const std::string& name, int width, int height);
    ~ProceduralSource() override;

    // --- TextureSource: Identity ---
    const UUID& getId() const override { return id_; }
    std::string getName() const override { return name_; }
    std::string getTypeName() const override { return "Procedural"; }

    // --- TextureSource: Lifecycle ---
    bool initialize() override;
    void shutdown() override;
    bool isReady() const override { return ready_; }

    // --- TextureSource: Per-Frame Update ---
    bool update(double deltaTime) override;

    // --- TextureSource: Frame Access (not used for GPU-native) ---
    bool hasNewFrame() const override { return dirty_; }
    const FrameData* getCurrentFrame() const override { return nullptr; }
    void markFrameConsumed() override { dirty_ = false; }

    // --- TextureSource: GPU-Native ---
    bool isGPUNative() const override { return true; }
    uint32_t getGLTextureId() const override { return outputTexture_; }

    // --- TextureSource: Capabilities ---
    SourceCapabilities getCapabilities() const override;

    // --- TextureSource: Dimensions ---
    int getWidth() const override { return width_; }
    int getHeight() const override { return height_; }

    // --- ProceduralSource: Parameters ---

    /**
     * @brief Get all adjustable parameters
     */
    virtual std::vector<SourceParameter> getParameters() const = 0;

    /**
     * @brief Set a parameter value
     * @param name Parameter name
     * @param value New value
     */
    virtual void setParameter(const std::string& name, const ParamValue& value) = 0;

    /**
     * @brief Reset all parameters to defaults
     */
    virtual void resetParameters() = 0;

    /**
     * @brief Mark the texture as needing regeneration
     */
    void markDirty() { dirty_ = true; }

    /**
     * @brief Check if this is an animated source
     */
    virtual bool isAnimated() const { return false; }

protected:
    /**
     * @brief Get the GLSL fragment shader source (#version 330 core)
     *
     * Subclasses must implement this to provide their shader code.
     * The shader writes its colour to `out vec4 fragColor`, with the texel
     * given by ivec2(gl_FragCoord.xy). u_Resolution and u_Time are set by the base.
     */
    virtual std::string getShaderSource() const = 0;

    /**
     * @brief Called before the draw to set shader uniforms
     *
     * Subclasses should set their parameter uniforms here.
     */
    virtual void setShaderUniforms() = 0;

    /**
     * @brief Called each frame for animated sources
     * @param deltaTime Time since last frame
     * @return true if the texture needs regeneration
     */
    virtual bool onUpdate(double deltaTime) { (void)deltaTime; return false; }

    /**
     * @brief Regenerate the texture
     */
    void regenerate();

    /**
     * @brief Create the output texture
     */
    void createOutputTexture();

    /**
     * @brief Destroy the output texture
     */
    void destroyOutputTexture();

    UUID id_ = UUID::generate();  // fresh id; null would make SourceManager reject it
    std::string name_;
    int width_ = 1920;
    int height_ = 1080;
    uint32_t outputTexture_ = 0;
    std::unique_ptr<ShaderProgram> shader_;
    uint32_t fbo_ = 0;  // draws into outputTexture_
    uint32_t vao_ = 0;  // empty; core profile needs one bound for the fullscreen triangle
    bool ready_ = false;
    bool dirty_ = true;
    double time_ = 0.0;  // Accumulated time for animations
};

} // namespace quadmapper
