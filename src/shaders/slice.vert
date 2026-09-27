#version 330 core

// A slice of a scene canvas drawn into an output frame (ADR-047 §4).
layout(location = 0) in vec2 a_Position;  // output-normalized [0,1], y down
layout(location = 1) in vec3 a_TexCoord;  // (u*q, v*q, q): the slice's own [0,1]

out vec3 v_TexCoordQ;

// Output rotation (QM-087): clockwise quarter turns (0-3), as in quad.vert.
uniform int u_OutputRotation;

void main() {
    vec2 ndc = a_Position * 2.0 - 1.0;
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
}
