#include "TestPatternSource.hpp"

namespace quadmapper {

TestPatternSource::TestPatternSource(int width, int height)
    : ProceduralSource("Test Pattern", width, height)
{
}

std::vector<SourceParameter> TestPatternSource::getParameters() const {
    return {
        {"patternType", "Pattern", SourceParameter::Type::Int, static_cast<int>(patternType_), 0, 0.0f, 3.0f}
    };
}

void TestPatternSource::setParameter(const std::string& name, const ParamValue& value) {
    if (name == "patternType") {
        if (auto* i = std::get_if<int>(&value)) {
            setPatternType(static_cast<PatternType>(*i));
        }
    }
}

void TestPatternSource::resetParameters() {
    patternType_ = PatternType::SMPTEBars;
    markDirty();
}

void TestPatternSource::setPatternType(PatternType type) {
    if (patternType_ != type) {
        patternType_ = type;
        markDirty();
    }
}

std::string TestPatternSource::getShaderSource() const {
    return R"(
#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba8, binding = 0) uniform image2D outputImage;

uniform vec2 u_Resolution;
uniform int u_PatternType;

// SMPTE color bar colors
const vec4 colors[7] = vec4[7](
    vec4(0.75, 0.75, 0.75, 1.0),  // White (75%)
    vec4(0.75, 0.75, 0.00, 1.0),  // Yellow
    vec4(0.00, 0.75, 0.75, 1.0),  // Cyan
    vec4(0.00, 0.75, 0.00, 1.0),  // Green
    vec4(0.75, 0.00, 0.75, 1.0),  // Magenta
    vec4(0.75, 0.00, 0.00, 1.0),  // Red
    vec4(0.00, 0.00, 0.75, 1.0)   // Blue
);

vec4 smpte(vec2 uv) {
    // Top 2/3: Color bars
    if (uv.y < 0.67) {
        int bar = int(uv.x * 7.0);
        bar = clamp(bar, 0, 6);
        return colors[bar];
    }
    // Middle section: Reverse bars (simplified)
    else if (uv.y < 0.75) {
        int bar = 6 - int(uv.x * 7.0);
        bar = clamp(bar, 0, 6);
        return colors[bar];
    }
    // Bottom: Grayscale ramp
    else {
        float gray = uv.x;
        return vec4(gray, gray, gray, 1.0);
    }
}

vec4 grid(vec2 uv) {
    float gridSize = 32.0;
    vec2 grid = fract(uv * gridSize);

    float lineWidth = 0.05;
    bool isLine = grid.x < lineWidth || grid.y < lineWidth;

    if (isLine) {
        return vec4(1.0, 1.0, 1.0, 1.0);
    }

    // Checkerboard background
    ivec2 cell = ivec2(floor(uv * gridSize));
    bool isEven = ((cell.x + cell.y) % 2) == 0;
    float gray = isEven ? 0.2 : 0.3;
    return vec4(gray, gray, gray, 1.0);
}

vec4 gradientPattern(vec2 uv) {
    // HSV to RGB gradient
    float h = uv.x;
    float s = 1.0;
    float v = 1.0 - uv.y * 0.5;

    // HSV to RGB conversion
    vec3 rgb;
    float c = v * s;
    float x = c * (1.0 - abs(mod(h * 6.0, 2.0) - 1.0));
    float m = v - c;

    if (h < 1.0/6.0) rgb = vec3(c, x, 0.0);
    else if (h < 2.0/6.0) rgb = vec3(x, c, 0.0);
    else if (h < 3.0/6.0) rgb = vec3(0.0, c, x);
    else if (h < 4.0/6.0) rgb = vec3(0.0, x, c);
    else if (h < 5.0/6.0) rgb = vec3(x, 0.0, c);
    else rgb = vec3(c, 0.0, x);

    rgb += m;
    return vec4(rgb, 1.0);
}

vec4 circle(vec2 uv) {
    vec2 center = uv - 0.5;
    float dist = length(center) * 2.0;

    // Concentric circles
    float rings = sin(dist * 20.0) * 0.5 + 0.5;

    // Center marker
    if (dist < 0.05) {
        return vec4(1.0, 0.0, 0.0, 1.0);
    }

    // Crosshair
    if (abs(center.x) < 0.005 || abs(center.y) < 0.005) {
        return vec4(1.0, 1.0, 1.0, 1.0);
    }

    return vec4(rings, rings, rings, 1.0);
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outputImage);

    if (pixel.x >= size.x || pixel.y >= size.y) {
        return;
    }

    vec2 uv = vec2(pixel) / u_Resolution;

    vec4 color;
    if (u_PatternType == 0) {
        color = smpte(uv);
    } else if (u_PatternType == 1) {
        color = grid(uv);
    } else if (u_PatternType == 2) {
        color = gradientPattern(uv);
    } else {
        color = circle(uv);
    }

    imageStore(outputImage, pixel, color);
}
)";
}

void TestPatternSource::setShaderUniforms() {
    shader_->setUniform("u_PatternType", static_cast<int>(patternType_));
}

} // namespace quadmapper
