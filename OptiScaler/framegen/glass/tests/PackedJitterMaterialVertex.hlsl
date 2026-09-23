// Fixture: the audited Cyberpunk transparent-VS camera block paired with a
// material that reads interpolants.
//
// PackedJitterVertex.hlsl emits SV_Position alone, which is enough for the
// UAV-writing fixture material (PackedUavMaterial.hlsl). The coverage-fallback
// check pairs the same capture with GeometryMaterialMrt.hlsl, whose pixel
// shader reads TEXCOORD0 and TEXCOORD1, so the vertex shader has to declare
// those rows. The production rewrite places the history block after every
// register the pixel shader reads, and a vertex shader that declares fewer
// interpolants than its pixel shader reads is not a pair the game produces.
// The synthetic coverage case therefore failed with
// "Pixel history register collision or overflow" on a fixture-only mismatch
// (baseline and candidate shader tools both, 2026-09-19 07:2x).
//
// The clip position is unchanged: id 2 sits on the 2x expanded triangle so uv
// stays in [0,1] across the viewport, which keeps the material's clip() and
// alpha exactly as the SV_Position-only fixture had them.
cbuffer Camera : register(b1, space0)
{
    float4 rows[53];
};

struct Output
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 tint : TEXCOORD1;
    float clip : SV_ClipDistance;
};

Output main(uint id : SV_VertexID)
{
    float2 p = id == 0 ? float2(-1, -1) : id == 1 ? float2(-1, 3) : float2(3, -1);
    // Keep the camera block live without moving the fixture's clip position:
    // the multiplier is zero for every vertex id the fixture submits.
    float zero = (id == 0xFFFFFFFFu) ? 1.0f : 0.0f;
    Output o;
    o.position = float4(p + rows[51].xy * zero, 0.5, 1);
    o.uv = p * 0.25 + 0.5;
    o.tint = float3(0.2, 0.5, 0.9);
    o.clip = 1;
    return o;
}
