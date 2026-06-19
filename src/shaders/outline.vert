#version 330 core

layout(location = 0) in vec2 a_Position;

// Editor view transform (ADR-041); defaults (1,1)/(0,0) = identity.
uniform vec2 u_ViewScale;
uniform vec2 u_ViewOffset;

void main() {
    vec2 view = a_Position * u_ViewScale + u_ViewOffset;
    vec2 ndc = view * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
