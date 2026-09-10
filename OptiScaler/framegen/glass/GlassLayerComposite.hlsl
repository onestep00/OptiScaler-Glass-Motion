// Experimental separated-layer composition. No runtime host enables this yet.
// All colors must share the original linear blend domain and exposure.
// Background is a separately generated FG image with the foreground removed.
// Endpoint UVs require verified surface correspondence, not background motion.
cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
    float Phase;
    uint InputsAdmitted;
};
Texture2D<float4> PreviousSource : register(t0);
Texture2D<float4> CurrentSource : register(t1);
Texture2D<float4> PreviousTransmission : register(t2);
Texture2D<float4> CurrentTransmission : register(t3);
Texture2D<float4> GeneratedBackground : register(t4);
Texture2D<float4> Fallback : register(t5);
Texture2D<float4> EndpointUVs : register(t6);
Texture2D<float4> CorrespondenceValid : register(t7);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void Composite(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;
    int3 p = int3(id.xy, 0);
    float4 fallback = Fallback.Load(p);
    Output[id.xy] = fallback;
    if (InputsAdmitted != 1 || !isfinite(Phase) || Phase < 0 || Phase > 1 ||
        CorrespondenceValid.Load(p).x != 1)
        return;
    float4 uv = EndpointUVs.Load(p);
    float2 halfTexel = 0.5 / float2(Width, Height);
    if (!all(isfinite(uv)) || any(uv.xy < halfTexel) || any(uv.zw < halfTexel) ||
        any(uv.xy > 1 - halfTexel) || any(uv.zw > 1 - halfTexel))
        return;
    float3 f0 = PreviousSource.SampleLevel(LinearClamp, uv.xy, 0).rgb;
    float3 f1 = CurrentSource.SampleLevel(LinearClamp, uv.zw, 0).rgb;
    float3 t0 = PreviousTransmission.SampleLevel(LinearClamp, uv.xy, 0).rgb;
    float3 t1 = CurrentTransmission.SampleLevel(LinearClamp, uv.zw, 0).rgb;
    float3 background = GeneratedBackground.Load(p).rgb;
    if (!all(isfinite(f0)) || !all(isfinite(f1)) || !all(isfinite(t0)) ||
        !all(isfinite(t1)) || !all(isfinite(background)) || any(t0 < 0) ||
        any(t1 < 0) || any(t0 > 1) || any(t1 > 1))
        return;
    float3 color = lerp(f0, f1, Phase) + lerp(t0, t1, Phase) * background;
    if (all(isfinite(color)))
        Output[id.xy] = float4(color, fallback.a);
}
