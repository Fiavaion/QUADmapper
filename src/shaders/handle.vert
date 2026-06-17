#version 330 core

layout(location = 0) in vec2 a_Position;

uniform vec2 u_HandleCenter;
uniform float u_HandleSize;
uniform float u_Aspect;  // framebuffer width / height; keeps handles round

void main() {
    // a_Position is a unit circle vertex (-1 to 1)
    // Transform to handle position in normalized coords. The normalized [0,1]
    // space is stretched horizontally by the framebuffer aspect ratio, so the
    // x extent is divided by u_Aspect to render a circle instead of an ellipse.
    vec2 worldPos = u_HandleCenter + vec2(a_Position.x * u_HandleSize / u_Aspect,
                                          a_Position.y * u_HandleSize);

    // Convert to NDC
    vec2 ndc = worldPos * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
