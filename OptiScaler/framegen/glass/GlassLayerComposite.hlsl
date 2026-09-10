// Experimental separated-layer composition. No runtime host enables this yet.
// Colors must share the original linear blend domain, exposure and frame pair.
// Each region is {originX, originY, width, height} in its own texture's texels.
// All regions describe the same view; normalized offsets include any jitter or
// projection correction. A size match does not establish semantic admission.
cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
    float Phase;
    uint InputsAdmitted;
    uint4 Regions[8];
};
Texture2D<float4> PreviousSource : register(t0);
Texture2D<float4> CurrentSource : register(t1);
Texture2D<float4> PreviousTransmission : register(t2);
Texture2D<float4> CurrentTransmission : register(t3);
Texture2D<float4> GeneratedBackground : register(t4);
Texture2D<float4> Fallback : register(t5);
// XY/ZW: offsets from this output pixel's normalized view UV to the two endpoints.
// Offsets can be filtered across grids without locking output pixels to the
// correspondence texture's texel centers.
Texture2D<float4> EndpointOffsets : register(t6);
// X: verified endpoint correspondence. Y: admitted correction footprint.
// The footprint must include displaced FG ghosts, not just surface coverage.
Texture2D<float4> Admission : register(t7);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Output : register(u0);

bool ValidRegion(Texture2D<float4> inputTexture, uint4 region)
{
    uint w, h;
    inputTexture.GetDimensions(w, h);
    return region.z > 0 && region.w > 0 && region.x <= w && region.y <= h &&
           region.z <= w - region.x && region.w <= h - region.y;
}

float4 SampleRegion(Texture2D<float4> inputTexture, uint4 region, float2 viewUV)
{
    uint w, h;
    inputTexture.GetDimensions(w, h);
    // Clamp to the valid region, not the allocation's possibly stale padding.
    float2 position = float2(region.xy) + viewUV * float2(region.zw);
    position = clamp(position, float2(region.xy) + 0.5,
                     float2(region.xy) + float2(region.zw) - 0.5);
    return inputTexture.SampleLevel(LinearClamp, position / float2(w, h), 0);
}

[numthreads(8, 8, 1)]
void Composite(uint3 id : SV_DispatchThreadID)
{
    uint outputWidth, outputHeight;
    Output.GetDimensions(outputWidth, outputHeight);
    if (id.x >= Width || id.y >= Height || id.x >= outputWidth || id.y >= outputHeight)
        return;
    // Host validation requires this exact-size fallback region before dispatch.
    if (!ValidRegion(Fallback, Regions[5]) || any(Regions[5].zw != uint2(Width, Height)))
        return;
    float4 fallback = Fallback.Load(int3(Regions[5].xy + id.xy, 0));
    Output[id.xy] = fallback;
    if (InputsAdmitted != 1 || !isfinite(Phase) || Phase < 0 || Phase > 1 ||
        !ValidRegion(PreviousSource, Regions[0]) || !ValidRegion(CurrentSource, Regions[1]) ||
        !ValidRegion(PreviousTransmission, Regions[2]) || !ValidRegion(CurrentTransmission, Regions[3]) ||
        !ValidRegion(GeneratedBackground, Regions[4]) || !ValidRegion(EndpointOffsets, Regions[6]) ||
        !ValidRegion(Admission, Regions[7]))
        return;
    float2 viewUV = (float2(id.xy) + 0.5) / float2(Width, Height);
    uint2 admissionPixel = min(uint2(viewUV * float2(Regions[7].zw)), Regions[7].zw - 1);
    if (any(Admission.Load(int3(Regions[7].xy + admissionPixel, 0)).xy != 1))
        return;
    float4 uv = viewUV.xyxy + SampleRegion(EndpointOffsets, Regions[6], viewUV);
    if (!all(isfinite(uv)) || any(uv < 0) || any(uv > 1))
        return;
    float3 f0 = SampleRegion(PreviousSource, Regions[0], uv.xy).rgb;
    float3 f1 = SampleRegion(CurrentSource, Regions[1], uv.zw).rgb;
    float3 t0 = SampleRegion(PreviousTransmission, Regions[2], uv.xy).rgb;
    float3 t1 = SampleRegion(CurrentTransmission, Regions[3], uv.zw).rgb;
    if (!all(isfinite(f0)) || !all(isfinite(f1)) || !all(isfinite(t0)) ||
        !all(isfinite(t1)) || any(t0 < 0) || any(t1 < 0) || any(t0 > 1) || any(t1 > 1))
        return;
    // Neutral layers inside the admitted footprint restore displaced ghosts.
    float3 background = SampleRegion(GeneratedBackground, Regions[4], viewUV).rgb;
    if (!all(isfinite(background)))
        return;
    float3 color = lerp(f0, f1, Phase) + lerp(t0, t1, Phase) * background;
    if (all(isfinite(color)))
        Output[id.xy] = float4(color, fallback.a);
}
