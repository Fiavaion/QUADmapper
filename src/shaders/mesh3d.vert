#version 330 core

// 3D mesh object seen from a projector (ADR-052). Pairs with quad.frag, so the
// crop, colour correction, opacity and colour-space paths are the quads' own.

layout(location = 0) in vec3 a_Position;  // model space
layout(location = 1) in vec2 a_TexCoord;

out vec3 v_TexCoordQ;  // (u, v, 1): quad.frag divides by .z, a no-op here
out vec3 v_ImageCoordQ;  // the same: an object samples its texture where it lies

// projection(near, far) * view * model, from the output's ProjectorCamera.
uniform mat4 u_MVP;

// Editor view transform (ADR-041), as in quad.vert: [0,1] canvas -> viewport region.
uniform vec2 u_ViewScale;
uniform vec2 u_ViewOffset;

// Output rotation (QM-087), as in quad.vert: clockwise quarter turns (0-3).
uniform int u_OutputRotation;

void main() {
    vec4 clip = u_MVP * vec4(a_Position, 1.0);

    // quad.vert maps canvas c to NDC n = (2c.x - 1, 1 - 2c.y) after c -> c*scale + offset.
    // In NDC that is n' = scale * n + (scale.x + 2 offset.x - 1, 1 - scale.y - 2 offset.y).
    // Applied to clip coordinates (the offset times w) it stays exact after the
    // perspective divide, and z/w are untouched, so depth is unchanged.
    vec2 shift = vec2(u_ViewScale.x + 2.0 * u_ViewOffset.x - 1.0,
                      1.0 - u_ViewScale.y - 2.0 * u_ViewOffset.y);
    vec2 xy = clip.xy * u_ViewScale + shift * clip.w;

    if (u_OutputRotation == 1) {
        xy = vec2(xy.y, -xy.x);
    } else if (u_OutputRotation == 2) {
        xy = -xy;
    } else if (u_OutputRotation == 3) {
        xy = vec2(-xy.y, xy.x);
    }
    gl_Position = vec4(xy, clip.z, clip.w);
    v_TexCoordQ = vec3(a_TexCoord, 1.0);
    v_ImageCoordQ = v_TexCoordQ;
}
