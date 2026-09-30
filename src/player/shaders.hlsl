// Draws a processor-decoded NV12 or P010 frame: two plane textures (Y, interleaved UV) sampled,
// converted to RGB by a 3x4 matrix that also carries range expansion and brightness, and scaled
// into the viewport (the fit/fill destination rectangle). Compiled at build time by fxc.

cbuffer Frame : register(b0) {
    float4 uvRect;  // source crop in texture coordinates: left, top, right, bottom
    float4 toR, toG, toB;  // rgb = dot(row, float4(y, u, v, 1))
};

Texture2D<float> lumaPlane : register(t0);
Texture2D<float2> chromaPlane : register(t1);
SamplerState linearClamp : register(s0);

struct Vertex {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// A quad from four vertex ids (triangle strip), no vertex buffer.
Vertex VSMain(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1);
    Vertex o;
    o.pos = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);
    o.uv = lerp(uvRect.xy, uvRect.zw, t);
    return o;
}

float4 PSMain(Vertex i) : SV_Target {
    float4 yuv = float4(lumaPlane.Sample(linearClamp, i.uv), chromaPlane.Sample(linearClamp, i.uv), 1);
    return float4(dot(toR, yuv), dot(toG, yuv), dot(toB, yuv), 1);
}
