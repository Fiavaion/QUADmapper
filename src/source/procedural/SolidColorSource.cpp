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
#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8, binding = 0) uniform image2D outputImage;

uniform vec4 u_Color;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputImage);

    if (pixel.x >= size.x || pixel.y >= size.y) {
        return;
    }

    imageStore(outputImage, pixel, u_Color);
}
)";
}

void SolidColorSource::setShaderUniforms() {
    shader_->setUniform("u_Color", color_);
}

} // namespace quadmapper
