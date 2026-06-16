#version 330 core

in vec2 v_TexCoord;
out vec4 FragColor;

uniform sampler2D u_Texture;
uniform float u_Opacity;
uniform vec2 u_UVOffset;
uniform vec2 u_UVScale;
uniform float u_UVRotation;
uniform int u_FlipH;
uniform int u_FlipV;

// Per-quad color correction (ADR-036). Identity = no change:
// gain (1,1,1), brightness 0, contrast 1, gamma 1.
uniform vec3 u_ColorRGBGain;
uniform float u_ColorBrightness;
uniform float u_ColorContrast;
uniform float u_ColorGamma;

void main() {
    // Start with base texture coordinates
    vec2 baseUV = v_TexCoord;

    // Apply flips first (before other transforms)
    if (u_FlipH == 1) {
        baseUV.x = 1.0 - baseUV.x;
    }
    if (u_FlipV == 1) {
        baseUV.y = 1.0 - baseUV.y;
    }

    // Center the UV coordinates around (0.5, 0.5) for transformations
    vec2 centered = baseUV - 0.5;

    // Apply rotation around center
    float cosR = cos(u_UVRotation);
    float sinR = sin(u_UVRotation);
    vec2 rotated = vec2(
        centered.x * cosR - centered.y * sinR,
        centered.x * sinR + centered.y * cosR
    );

    // Apply scale (centered) and offset, then recenter
    vec2 uv = rotated * u_UVScale + 0.5 + u_UVOffset;

    vec4 color = texture(u_Texture, uv);

    // Color correction, applied in order: RGB gain -> brightness -> contrast ->
    // gamma. Identity uniforms make every step a no-op (ADR-036).
    vec3 rgb = color.rgb * u_ColorRGBGain;
    rgb += u_ColorBrightness;
    rgb = (rgb - 0.5) * u_ColorContrast + 0.5;
    rgb = pow(max(rgb, 0.0), vec3(1.0 / u_ColorGamma));
    color.rgb = rgb;

    color.a *= u_Opacity;
    FragColor = color;
}
