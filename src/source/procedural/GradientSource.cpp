#include "GradientSource.hpp"
#include <cmath>

namespace quadmapper {

GradientSource::GradientSource(int width, int height)
    : ProceduralSource("Gradient", width, height)
{
}

std::vector<SourceParameter> GradientSource::getParameters() const {
    return {
        {"startColor", "Start Color", SourceParameter::Type::Color, startColor_, Color(1.0f, 0.0f, 0.0f, 1.0f), 0.0f, 1.0f},
        {"endColor", "End Color", SourceParameter::Type::Color, endColor_, Color(0.0f, 0.0f, 1.0f, 1.0f), 0.0f, 1.0f},
        {"angle", "Angle", SourceParameter::Type::Float, angle_, 0.0f, 0.0f, 360.0f},
        {"gradientType", "Type", SourceParameter::Type::Int, static_cast<int>(gradientType_), 0, 0.0f, 3.0f},
        {"animate", "Animate", SourceParameter::Type::Bool, animate_, false, 0.0f, 1.0f},
        {"animationSpeed", "Speed", SourceParameter::Type::Float, animationSpeed_, 30.0f, 0.0f, 360.0f}
    };
}

void GradientSource::setParameter(const std::string& name, const ParamValue& value) {
    if (name == "startColor") {
        if (auto* c = std::get_if<Color>(&value)) {
            setStartColor(*c);
        }
    } else if (name == "endColor") {
        if (auto* c = std::get_if<Color>(&value)) {
            setEndColor(*c);
        }
    } else if (name == "angle") {
        if (auto* f = std::get_if<float>(&value)) {
            setAngle(*f);
        }
    } else if (name == "gradientType") {
        if (auto* i = std::get_if<int>(&value)) {
            setGradientType(static_cast<GradientType>(*i));
        }
    } else if (name == "animate") {
        if (auto* b = std::get_if<bool>(&value)) {
            setAnimate(*b);
        }
    } else if (name == "animationSpeed") {
        if (auto* f = std::get_if<float>(&value)) {
            animationSpeed_ = *f;
        }
    }
}

void GradientSource::resetParameters() {
    startColor_ = Color(1.0f, 0.0f, 0.0f, 1.0f);
    endColor_ = Color(0.0f, 0.0f, 1.0f, 1.0f);
    angle_ = 0.0f;
    gradientType_ = GradientType::Linear;
    animate_ = false;
    animationSpeed_ = 30.0f;
    markDirty();
}

void GradientSource::setStartColor(const Color& color) {
    if (startColor_ != color) {
        startColor_ = color;
        markDirty();
    }
}

void GradientSource::setEndColor(const Color& color) {
    if (endColor_ != color) {
        endColor_ = color;
        markDirty();
    }
}

void GradientSource::setAngle(float degrees) {
    if (angle_ != degrees) {
        angle_ = degrees;
        markDirty();
    }
}

void GradientSource::setGradientType(GradientType type) {
    if (gradientType_ != type) {
        gradientType_ = type;
        markDirty();
    }
}

void GradientSource::setAnimate(bool animate) {
    animate_ = animate;
}

bool GradientSource::onUpdate(double deltaTime) {
    if (animate_) {
        angle_ += static_cast<float>(deltaTime * animationSpeed_);
        if (angle_ >= 360.0f) {
            angle_ = std::fmod(angle_, 360.0f);
        }
        return true;
    }
    return false;
}

std::string GradientSource::getShaderSource() const {
    return R"(
#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8, binding = 0) uniform image2D outputImage;

uniform vec2 u_Resolution;
uniform vec4 u_StartColor;
uniform vec4 u_EndColor;
uniform float u_Angle;
uniform int u_GradientType;

const float PI = 3.14159265359;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputImage);

    if (pixel.x >= size.x || pixel.y >= size.y) {
        return;
    }

    vec2 uv = vec2(pixel) / u_Resolution;
    vec2 center = uv - 0.5;

    float t = 0.0;
    float angleRad = u_Angle * PI / 180.0;

    if (u_GradientType == 0) {
        // Linear gradient
        float cosA = cos(angleRad);
        float sinA = sin(angleRad);
        t = center.x * cosA + center.y * sinA + 0.5;
    } else if (u_GradientType == 1) {
        // Radial gradient
        t = length(center) * 2.0;
    } else if (u_GradientType == 2) {
        // Angular gradient
        float angle = atan(center.y, center.x);
        t = (angle + PI + angleRad) / (2.0 * PI);
        t = fract(t);
    } else if (u_GradientType == 3) {
        // Diamond gradient
        vec2 rotated;
        rotated.x = center.x * cos(angleRad) - center.y * sin(angleRad);
        rotated.y = center.x * sin(angleRad) + center.y * cos(angleRad);
        t = (abs(rotated.x) + abs(rotated.y)) * 2.0;
    }

    t = clamp(t, 0.0, 1.0);
    vec4 color = mix(u_StartColor, u_EndColor, t);

    imageStore(outputImage, pixel, color);
}
)";
}

void GradientSource::setShaderUniforms() {
    shader_->setUniform("u_StartColor", startColor_);
    shader_->setUniform("u_EndColor", endColor_);
    shader_->setUniform("u_Angle", angle_);
    shader_->setUniform("u_GradientType", static_cast<int>(gradientType_));
}

} // namespace quadmapper
