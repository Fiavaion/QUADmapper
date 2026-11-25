#include "CheckerboardSource.hpp"

namespace quadmapper {

CheckerboardSource::CheckerboardSource(int width, int height)
    : ProceduralSource("Checkerboard", width, height)
{
}

std::vector<SourceParameter> CheckerboardSource::getParameters() const {
    return {
        {"color1", "Color 1", SourceParameter::Type::Color, color1_, Color(0.0f, 0.0f, 0.0f, 1.0f), 0.0f, 1.0f},
        {"color2", "Color 2", SourceParameter::Type::Color, color2_, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.0f, 1.0f},
        {"size", "Size", SourceParameter::Type::Float, size_, 8.0f, 1.0f, 64.0f}
    };
}

void CheckerboardSource::setParameter(const std::string& name, const ParamValue& value) {
    if (name == "color1") {
        if (auto* c = std::get_if<Color>(&value)) {
            setColor1(*c);
        }
    } else if (name == "color2") {
        if (auto* c = std::get_if<Color>(&value)) {
            setColor2(*c);
        }
    } else if (name == "size") {
        if (auto* f = std::get_if<float>(&value)) {
            setSize(*f);
        }
    }
}

void CheckerboardSource::resetParameters() {
    color1_ = Color(0.0f, 0.0f, 0.0f, 1.0f);
    color2_ = Color(1.0f, 1.0f, 1.0f, 1.0f);
    size_ = 8.0f;
    markDirty();
}

void CheckerboardSource::setColor1(const Color& color) {
    if (color1_ != color) {
        color1_ = color;
        markDirty();
    }
}

void CheckerboardSource::setColor2(const Color& color) {
    if (color2_ != color) {
        color2_ = color;
        markDirty();
    }
}

void CheckerboardSource::setSize(float size) {
    if (size_ != size) {
        size_ = size;
        markDirty();
    }
}

std::string CheckerboardSource::getShaderSource() const {
    return R"(
#version 330 core
out vec4 fragColor;

uniform vec2 u_Resolution;
uniform vec4 u_Color1;
uniform vec4 u_Color2;
uniform float u_Size;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);

    vec2 uv = vec2(pixel) / u_Resolution;

    // Calculate checker pattern
    ivec2 checker = ivec2(floor(uv * u_Size));
    bool isEven = ((checker.x + checker.y) % 2) == 0;

    vec4 color = isEven ? u_Color1 : u_Color2;

    fragColor = color;
}
)";
}

void CheckerboardSource::setShaderUniforms() {
    shader_->setVec4("u_Color1", color1_);
    shader_->setVec4("u_Color2", color2_);
    shader_->setFloat("u_Size", size_);
}

} // namespace quadmapper
