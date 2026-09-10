// Independent rasterized fixture. Not a replacement for a game material shader.
#include "../GlassObjectMotion.hlsli"
cbuffer ObjectData : register(b0)
{
    row_major float4x4 CurrentObject;
    row_major float4x4 PreviousObject;
    row_major float4x4 CurrentCamera;
    row_major float4x4 PreviousCamera;
    float4 Raster; // Current jitter UV, opacity, fixture object ID.
    float4 Options; // Optional UV discard hole, opacity gain, opacity bias, unused.
};
struct Vertex
{
    float3 current : POSITION0;
    float3 previous : POSITION1;
    float2 uv : TEXCOORD0;
};
struct Interpolants
{
    float4 raster : SV_Position;
    float4 currentClip : TEXCOORD0;
    float4 previousClip : TEXCOORD1;
    float2 uv : TEXCOORD2;
};
Interpolants VS(Vertex v)
{
    Interpolants o;
    o.currentClip = GlassObjectClip(v.current, CurrentObject, CurrentCamera);
    o.previousClip = GlassObjectClip(v.previous, PreviousObject, PreviousCamera);
    o.raster = o.currentClip;
    o.raster.xy += Raster.xy * float2(2, -2) * o.raster.w;
    o.uv = v.uv;
    return o;
}
struct Targets
{
    float4 motionDepth : SV_Target0; // UV displacement, current depth, validity.
    float4 coverage : SV_Target1; // Object ID, opacity, interpolated material UV.
    float4 weights : SV_Target2; // Interior, confirmed edge, half edge, unused.
};
Targets PS(Interpolants p)
{
    if (Options.x != 0 && length(p.uv - .5) < .15)
        discard;
    Targets o;
    float2 motion;
    bool valid = GlassObjectMotion(p.currentClip, p.previousClip, motion);
    o.motionDepth = float4(valid ? motion : float2(0, 0), p.currentClip.z / p.currentClip.w, valid ? 1 : 0);
    o.coverage = float4(Raster.w, Raster.z, p.uv);
    o.weights = float4(GlassMotionWeight(Raster.z, 0, Options.y, Options.z),
                      GlassMotionWeight(Raster.z, 1, Options.y, Options.z),
                      GlassMotionWeight(Raster.z, .5, Options.y, Options.z), 0);
    return o;
}
