#version 330 core

layout(location = 0) in vec2 a_Position;

uniform vec2 u_HandleCenter;
uniform float u_HandleSize;
uniform float u_Aspect;  // canvas pixel aspect (W/H); keeps handles round

// Editor view transform (ADR-041); defaults (1,1)/(0,0) = identity.
uniform vec2 u_ViewScale;
uniform vec2 u_ViewOffset;

void main() {
    // a_Position is a unit circle vertex (-1 to 1). The handle center + offset are
    // in [0,1] canvas space; the x extent is divided by u_Aspect so that, once the
    // view transform letterboxes the canvas to that same aspect, the handle renders
    // as a circle instead of an ellipse.
    vec2 canvasPos = u_HandleCenter + vec2(a_Position.x * u_HandleSize / u_Aspect,
                                           a_Position.y * u_HandleSize);

    // Apply the editor view transform, then convert to NDC.
    vec2 view = canvasPos * u_ViewScale + u_ViewOffset;
    vec2 ndc = view * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
