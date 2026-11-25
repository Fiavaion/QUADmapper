#include "NoiseSource.hpp"

namespace quadmapper {

NoiseSource::NoiseSource(int width, int height)
    : ProceduralSource("Noise", width, height)
{
}

std::vector<SourceParameter> NoiseSource::getParameters() const {
    return {
        {"scale", "Scale", SourceParameter::Type::Float, scale_, 4.0f, 0.1f, 32.0f},
        {"octaves", "Octaves", SourceParameter::Type::Int, octaves_, 4, 1.0f, 8.0f},
        {"speed", "Speed", SourceParameter::Type::Float, speed_, 0.5f, 0.0f, 5.0f},
        {"animate", "Animate", SourceParameter::Type::Bool, animate_, true, 0.0f, 1.0f},
        {"color1", "Color 1", SourceParameter::Type::Color, color1_, Color(0.0f, 0.0f, 0.0f, 1.0f), 0.0f, 1.0f},
        {"color2", "Color 2", SourceParameter::Type::Color, color2_, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.0f, 1.0f}
    };
}

void NoiseSource::setParameter(const std::string& name, const ParamValue& value) {
    if (name == "scale") {
        if (auto* f = std::get_if<float>(&value)) {
            setScale(*f);
        }
    } else if (name == "octaves") {
        if (auto* i = std::get_if<int>(&value)) {
            setOctaves(*i);
        }
    } else if (name == "speed") {
        if (auto* f = std::get_if<float>(&value)) {
            setSpeed(*f);
        }
    } else if (name == "animate") {
        if (auto* b = std::get_if<bool>(&value)) {
            setAnimate(*b);
        }
    } else if (name == "color1") {
        if (auto* c = std::get_if<Color>(&value)) {
            setColor1(*c);
        }
    } else if (name == "color2") {
        if (auto* c = std::get_if<Color>(&value)) {
            setColor2(*c);
        }
    }
}

void NoiseSource::resetParameters() {
    scale_ = 4.0f;
    octaves_ = 4;
    speed_ = 0.5f;
    animate_ = true;
    color1_ = Color(0.0f, 0.0f, 0.0f, 1.0f);
    color2_ = Color(1.0f, 1.0f, 1.0f, 1.0f);
    markDirty();
}

void NoiseSource::setScale(float scale) {
    if (scale_ != scale) {
        scale_ = scale;
        markDirty();
    }
}

void NoiseSource::setOctaves(int octaves) {
    if (octaves_ != octaves) {
        octaves_ = octaves;
        markDirty();
    }
}

void NoiseSource::setSpeed(float speed) {
    speed_ = speed;
}

void NoiseSource::setAnimate(bool animate) {
    animate_ = animate;
}

void NoiseSource::setColor1(const Color& color) {
    if (color1_ != color) {
        color1_ = color;
        markDirty();
    }
}

void NoiseSource::setColor2(const Color& color) {
    if (color2_ != color) {
        color2_ = color;
        markDirty();
    }
}

bool NoiseSource::onUpdate(double /*deltaTime*/) {
    // Always regenerate if animating (time_ is accumulated in base class)
    return animate_;
}

std::string NoiseSource::getShaderSource() const {
    return R"(
#version 330 core
out vec4 fragColor;

uniform vec2 u_Resolution;
uniform float u_Time;
uniform float u_Scale;
uniform int u_Octaves;
uniform float u_Speed;
uniform vec4 u_Color1;
uniform vec4 u_Color2;

// Simplex 3D Noise (adapted from Ashima Arts)
vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 mod289(vec4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 permute(vec4 x) { return mod289(((x*34.0)+1.0)*x); }
vec4 taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }

float snoise(vec3 v) {
    const vec2 C = vec2(1.0/6.0, 1.0/3.0);
    const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);

    vec3 i = floor(v + dot(v, C.yyy));
    vec3 x0 = v - i + dot(i, C.xxx);

    vec3 g = step(x0.yzx, x0.xyz);
    vec3 l = 1.0 - g;
    vec3 i1 = min(g.xyz, l.zxy);
    vec3 i2 = max(g.xyz, l.zxy);

    vec3 x1 = x0 - i1 + C.xxx;
    vec3 x2 = x0 - i2 + C.yyy;
    vec3 x3 = x0 - D.yyy;

    i = mod289(i);
    vec4 p = permute(permute(permute(
        i.z + vec4(0.0, i1.z, i2.z, 1.0))
        + i.y + vec4(0.0, i1.y, i2.y, 1.0))
        + i.x + vec4(0.0, i1.x, i2.x, 1.0));

    float n_ = 0.142857142857;
    vec3 ns = n_ * D.wyz - D.xzx;

    vec4 j = p - 49.0 * floor(p * ns.z * ns.z);

    vec4 x_ = floor(j * ns.z);
    vec4 y_ = floor(j - 7.0 * x_);

    vec4 x = x_ * ns.x + ns.yyyy;
    vec4 y = y_ * ns.x + ns.yyyy;
    vec4 h = 1.0 - abs(x) - abs(y);

    vec4 b0 = vec4(x.xy, y.xy);
    vec4 b1 = vec4(x.zw, y.zw);

    vec4 s0 = floor(b0)*2.0 + 1.0;
    vec4 s1 = floor(b1)*2.0 + 1.0;
    vec4 sh = -step(h, vec4(0.0));

    vec4 a0 = b0.xzyw + s0.xzyw*sh.xxyy;
    vec4 a1 = b1.xzyw + s1.xzyw*sh.zzww;

    vec3 p0 = vec3(a0.xy, h.x);
    vec3 p1 = vec3(a0.zw, h.y);
    vec3 p2 = vec3(a1.xy, h.z);
    vec3 p3 = vec3(a1.zw, h.w);

    vec4 norm = taylorInvSqrt(vec4(dot(p0,p0), dot(p1,p1), dot(p2,p2), dot(p3,p3)));
    p0 *= norm.x;
    p1 *= norm.y;
    p2 *= norm.z;
    p3 *= norm.w;

    vec4 m = max(0.6 - vec4(dot(x0,x0), dot(x1,x1), dot(x2,x2), dot(x3,x3)), 0.0);
    m = m * m;
    return 42.0 * dot(m*m, vec4(dot(p0,x0), dot(p1,x1), dot(p2,x2), dot(p3,x3)));
}

float fbm(vec3 p, int octaves) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    for (int i = 0; i < octaves; i++) {
        value += amplitude * snoise(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }

    return value;
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);

    vec2 uv = vec2(pixel) / u_Resolution;
    vec3 p = vec3(uv * u_Scale, u_Time * u_Speed);

    float n = fbm(p, u_Octaves);
    n = n * 0.5 + 0.5;  // Map from [-1,1] to [0,1]

    vec4 color = mix(u_Color1, u_Color2, n);

    fragColor = color;
}
)";
}

void NoiseSource::setShaderUniforms() {
    shader_->setFloat("u_Scale", scale_);
    shader_->setInt("u_Octaves", octaves_);
    shader_->setFloat("u_Speed", speed_);
    shader_->setVec4("u_Color1", color1_);
    shader_->setVec4("u_Color2", color2_);
}

} // namespace quadmapper
