RWByteAddressBuffer OriginalWrites : register(u3);
float4 main(float4 p : SV_Position) : SV_Target
{
    uint2 xy = uint2(p.xy);
    clip(p.x - 8.0);
    if (xy.x == 20) discard;
    uint ignored;
    OriginalWrites.InterlockedAdd(0, 1, ignored);
    OriginalWrites.Store(4 + (xy.y * 64 + xy.x) * 4, 1 + xy.y * 64 + xy.x);
    return float4(0.25, 0.5, 0.75, 0.5);
}
