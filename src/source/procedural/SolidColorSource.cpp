#include "SolidColorSource.hpp"

namespace quadmapper {

SolidColorSource::SolidColorSource(int width, int height)
    : ProceduralSource("Solid Color", width, height)
{
}

std::vector<SourceParameter> SolidColorSource::getParameters() const {
    return {
        {"color", "Color", SourceParameter::Type::Color, color_, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.0f, 1.0f}
    };
}

void SolidColorSource::setParameter(const std::string& name, const ParamValue& value) {
    if (name == "color") {
        if (auto* c = std::get_if<Color>(&value)) {
            setColor(*c);
        }
    }
}

void SolidColorSource::resetParameters() {
    color_ = Color(1.0f, 1.0f, 1.0f, 1.0f);
    markDirty();
}

void SolidColorSource::setColor(const Color& color) {
    if (color_ != color) {
        color_ = color;
        markDirty();
    }
}

std::string SolidColorSource::getShaderSource() const {
    return R"(
#version 330 core
out vec4 fragColor;

uniform vec4 u_Color;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);

    fragColor = u_Color;
}
)";
}

void SolidColorSource::setShaderUniforms() {
    shader_->setVec4("u_Color", color_);
}

} // namespace quadmapper
