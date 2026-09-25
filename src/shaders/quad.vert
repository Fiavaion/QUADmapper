#version 330 core

layout(location = 0) in vec2 a_Position;
layout(location = 1) in vec3 a_TexCoord;  // (u*q, v*q, q)

// Projective texture coordinate: interpolated linearly across the triangle and
// divided by .z per fragment, which reproduces the corner homography exactly.
out vec3 v_TexCoordQ;

// Editor view transform (ADR-041). a_Position is in [0,1] canvas space; the view
// maps it into the viewport region. Defaults (1,1)/(0,0) = full-region identity,
// which is what the per-output windows and preview FBOs always use.
uniform vec2 u_ViewScale;
uniform vec2 u_ViewOffset;

void main() {
    vec2 view = a_Position * u_ViewScale + u_ViewOffset;
    vec2 ndc = view * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_TexCoordQ = a_TexCoord;
}
