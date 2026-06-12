#version 330 core

layout(location = 0) in vec2 a_Position;

uniform vec2 u_HandleCenter;
uniform float u_HandleSize;

void main() {
    // a_Position is a unit circle vertex (-1 to 1)
    // Transform to handle position in normalized coords
    vec2 worldPos = u_HandleCenter + a_Position * u_HandleSize;

    // Convert to NDC
    vec2 ndc = worldPos * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
