// A material pixel shader that exports a colour target plus SV_Depth.
//
// The packed rewrite keeps depth exports only on the coverage-only variant, so
// the material pass of this shader is rejected with "Pixel depth/stencil
// exports require separate coverage validation" and the coverage recovery of
// GeometryPipeline::createPackedMotion is the only path that accepts it. This
// is the fixture input that makes the packed coverage fallback counter
// observable; GeometryMaterialMrt.hlsl is kept as the control, because extra
// MRT slots above zero are skipped rather than rejected.
//
// Inputs match PackedJitterMaterialVertex.hlsl (SV_Position, TEXCOORD0,
// TEXCOORD1): the pair is the vertex/pixel relation the game produces. A pixel
// shader with no inputs throws "Unexpected metadata type" in the same rewrite
// helper, which is a fixture limitation rather than a product path.
struct Output
{
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

Output main(float4 p : SV_Position, float2 uv : TEXCOORD0, float3 tint : TEXCOORD1)
{
    float a = saturate(1.0 - length((uv - 0.5) * 2.0));
    clip(a - 0.13);
    clip(abs(uv.x - 0.5) - 0.05);
    Output o;
    o.color = float4(tint * (0.5 + uv.y * 0.5), a);
    o.depth = p.z;
    return o;
}
