#include "PlasmaSource.hpp"

namespace quadmapper {

PlasmaSource::PlasmaSource(int width, int height)
    : ProceduralSource("Plasma", width, height)
{
}

std::vector<SourceParameter> PlasmaSource::getParameters() const {
    return {
        {"speed", "Speed", SourceParameter::Type::Float, speed_, 1.0f, 0.0f, 5.0f},
        {"complexity", "Complexity", SourceParameter::Type::Float, complexity_, 3.0f, 1.0f, 10.0f},
        {"color1", "Color 1", SourceParameter::Type::Color, color1_, Color(1.0f, 0.0f, 0.0f, 1.0f), 0.0f, 1.0f},
        {"color2", "Color 2", SourceParameter::Type::Color, color2_, Color(0.0f, 1.0f, 0.0f, 1.0f), 0.0f, 1.0f},
        {"color3", "Color 3", SourceParameter::Type::Color, color3_, Color(0.0f, 0.0f, 1.0f, 1.0f), 0.0f, 1.0f}
    };
}

void PlasmaSource::setParameter(const std::string& name, const ParamValue& value) {
    if (name == "speed") {
        if (auto* f = std::get_if<float>(&value)) {
            setSpeed(*f);
        }
    } else if (name == "complexity") {
        if (auto* f = std::get_if<float>(&value)) {
            setComplexity(*f);
        }
    } else if (name == "color1") {
        if (auto* c = std::get_if<Color>(&value)) {
            setColor1(*c);
        }
    } else if (name == "color2") {
        if (auto* c = std::get_if<Color>(&value)) {
            setColor2(*c);
        }
    } else if (name == "color3") {
        if (auto* c = std::get_if<Color>(&value)) {
            setColor3(*c);
        }
    }
}

void PlasmaSource::resetParameters() {
    speed_ = 1.0f;
    complexity_ = 3.0f;
    color1_ = Color(1.0f, 0.0f, 0.0f, 1.0f);
    color2_ = Color(0.0f, 1.0f, 0.0f, 1.0f);
    color3_ = Color(0.0f, 0.0f, 1.0f, 1.0f);
    markDirty();
}

void PlasmaSource::setSpeed(float speed) {
    speed_ = speed;
}

void PlasmaSource::setComplexity(float complexity) {
    if (complexity_ != complexity) {
        complexity_ = complexity;
        markDirty();
    }
}

void PlasmaSource::setColor1(const Color& color) {
    if (color1_ != color) {
        color1_ = color;
        markDirty();
    }
}

void PlasmaSource::setColor2(const Color& color) {
    if (color2_ != color) {
        color2_ = color;
        markDirty();
    }
}

void PlasmaSource::setColor3(const Color& color) {
    if (color3_ != color) {
        color3_ = color;
        markDirty();
    }
}

bool PlasmaSource::onUpdate(double /*deltaTime*/) {
    // Always regenerate (animated)
    return true;
}

std::string PlasmaSource::getShaderSource() const {
    return R"(
#version 330 core
out vec4 fragColor;

uniform vec2 u_Resolution;
uniform float u_Time;
uniform float u_Speed;
uniform float u_Complexity;
uniform vec4 u_Color1;
uniform vec4 u_Color2;
uniform vec4 u_Color3;

const float PI = 3.14159265359;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);

    vec2 uv = vec2(pixel) / u_Resolution;
    float time = u_Time * u_Speed;

    // Classic plasma formula
    float v1 = sin(uv.x * u_Complexity + time);
    float v2 = sin(u_Complexity * (uv.x * sin(time * 0.5) + uv.y * cos(time * 0.3)) + time);

    float cx = uv.x + 0.5 * sin(time * 0.2);
    float cy = uv.y + 0.5 * cos(time * 0.3);
    float v3 = sin(sqrt((cx * cx + cy * cy) * u_Complexity * 2.0) + time);

    float v4 = sin(sqrt((uv.x * uv.x + uv.y * uv.y) * u_Complexity) + time);

    float v = (v1 + v2 + v3 + v4) * 0.25;
    v = v * 0.5 + 0.5;  // Normalize to [0, 1]

    // Three-color gradient
    vec4 color;
    if (v < 0.5) {
        color = mix(u_Color1, u_Color2, v * 2.0);
    } else {
        color = mix(u_Color2, u_Color3, (v - 0.5) * 2.0);
    }

    fragColor = color;
}
)";
}

void PlasmaSource::setShaderUniforms() {
    shader_->setFloat("u_Speed", speed_);
    shader_->setFloat("u_Complexity", complexity_);
    shader_->setVec4("u_Color1", color1_);
    shader_->setVec4("u_Color2", color2_);
    shader_->setVec4("u_Color3", color3_);
}

} // namespace quadmapper
