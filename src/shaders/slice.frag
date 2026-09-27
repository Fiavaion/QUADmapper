#version 330 core

// A slice of a scene canvas (ADR-047 §4): the input rectangle, faded by the edge
// blend zones and lifted by the black level. The canvas holds colour already blended
// over black with coverage in alpha, so the frame composites it with
// (ONE, ONE_MINUS_SRC_ALPHA). The ramp matches sliceBlendGain in scene/Slice.cpp.

in vec3 v_TexCoordQ;
out vec4 FragColor;

uniform sampler2D u_Canvas;
uniform vec2 u_CanvasSize;   // canvas pixels, unrotated
// The canvas texture holds the canvas turned by this many clockwise quarter turns
// (0-3), drawn exactly as quad.vert turns an output (so one rotated projector's
// canvas is rasterized as its frame was before canvases).
uniform int u_CanvasRotation;
uniform vec4 u_Input;        // input rectangle x, y, width, height in canvas pixels, y down
uniform int u_Exact;         // 1: the output pixel is the canvas texel under it, unfiltered
uniform vec4 u_BlendWidth;   // left, right, top, bottom zone widths in canvas pixels (0 = off)
uniform vec4 u_BlendGamma;
uniform vec4 u_BlendCurve;
uniform float u_BlackLevel;

// Linear-light ramp: ramp(t) + ramp(1 - t) == 1 for every curve p.
float ramp(float t, float p) {
    t = clamp(t, 0.0, 1.0);
    return t < 0.5 ? 0.5 * pow(2.0 * t, p) : 1.0 - 0.5 * pow(2.0 * (1.0 - t), p);
}

float edgeGain(float distance, float width, float gamma, float curve) {
    if (width <= 0.0 || distance >= width) return 1.0;
    return pow(ramp(distance / width, curve), 1.0 / gamma);
}

void main() {
    vec2 uv = v_TexCoordQ.xy / v_TexCoordQ.z;
    vec2 inSlice = uv * u_Input.zw;
    vec2 canvasPx = u_Input.xy + inSlice;

    // Where that canvas point is in the texture: the mapping quad.vert used to draw
    // it (y flipped to the GL origin, then the canvas rotation).
    vec2 canvasN = canvasPx / u_CanvasSize;
    vec2 ndc = vec2(canvasN.x * 2.0 - 1.0, 1.0 - canvasN.y * 2.0);
    if (u_CanvasRotation == 1) {
        ndc = vec2(ndc.y, -ndc.x);
    } else if (u_CanvasRotation == 2) {
        ndc = -ndc;
    } else if (u_CanvasRotation == 3) {
        ndc = vec2(-ndc.y, ndc.x);
    }
    vec2 t = ndc * 0.5 + 0.5;
    vec2 texSize = (u_CanvasRotation == 1 || u_CanvasRotation == 3) ? u_CanvasSize.yx : u_CanvasSize;
    vec4 c;
    if (u_Exact == 1) {
        ivec2 texel = clamp(ivec2(floor(t * texSize)), ivec2(0), ivec2(texSize) - 1);
        c = texelFetch(u_Canvas, texel, 0);
    } else {
        c = texture(u_Canvas, t);
    }

    vec4 d = vec4(inSlice.x, u_Input.z - inSlice.x, inSlice.y, u_Input.w - inSlice.y);
    float gain = edgeGain(d.x, u_BlendWidth.x, u_BlendGamma.x, u_BlendCurve.x) *
                 edgeGain(d.y, u_BlendWidth.y, u_BlendGamma.y, u_BlendCurve.y) *
                 edgeGain(d.z, u_BlendWidth.z, u_BlendGamma.z, u_BlendCurve.z) *
                 edgeGain(d.w, u_BlendWidth.w, u_BlendGamma.w, u_BlendCurve.w);

    vec3 rgb = c.rgb * gain;
    float alpha = c.a;
    if (u_BlackLevel > 0.0 && all(greaterThanEqual(d, u_BlendWidth))) {
        // The frame becomes L + (1 - L) * (canvas over what is already drawn, the 3D
        // objects): colour L + (1 - L) c.rgb, and the frame below kept by
        // (1 - L)(1 - c.a), which the (ONE, ONE_MINUS_SRC_ALPHA) blend reads from alpha.
        rgb = u_BlackLevel + (1.0 - u_BlackLevel) * rgb;
        alpha = 1.0 - (1.0 - u_BlackLevel) * (1.0 - c.a);
    }
    FragColor = vec4(rgb, alpha);
}
