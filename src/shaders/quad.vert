#version 330 core

layout(location = 0) in vec2 a_Position;
layout(location = 1) in vec3 a_TexCoord;  // (u*q, v*q, q)
layout(location = 2) in vec3 a_ImageCoord;  // where the image is sampled, same form

// Projective texture coordinate: interpolated linearly across the triangle and
// divided by .z per fragment, which reproduces the corner homography exactly.
out vec3 v_TexCoordQ;
// The image coordinate: equal to v_TexCoordQ, except in Crop mode, where the image
// stays on the plain corner pin and the bent outline cuts it.
out vec3 v_ImageCoordQ;

// Editor view transform (ADR-041). a_Position is in [0,1] canvas space; the view
// maps it into the viewport region. Defaults (1,1)/(0,0) = full-region identity,
// which is what the per-output windows and preview FBOs always use.
uniform vec2 u_ViewScale;
uniform vec2 u_ViewOffset;

// Output rotation (QM-087): clockwise quarter turns (0-3) of the whole frame, for
// a projector mounted on its side. 0 (the unset default) = no rotation.
uniform int u_OutputRotation;

void main() {
    vec2 view = a_Position * u_ViewScale + u_ViewOffset;
    vec2 ndc = view * 2.0 - 1.0;
    ndc.y = -ndc.y;
    if (u_OutputRotation == 1) {
        ndc = vec2(ndc.y, -ndc.x);
    } else if (u_OutputRotation == 2) {
        ndc = -ndc;
    } else if (u_OutputRotation == 3) {
        ndc = vec2(-ndc.y, ndc.x);
    }
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_TexCoordQ = a_TexCoord;
    v_ImageCoordQ = a_ImageCoord;
}
