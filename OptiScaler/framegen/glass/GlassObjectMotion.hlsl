ByteAddressBuffer ObjectMotion : register(t0);
RWTexture2D<float4> Motion : register(u0);
RWTexture2D<float> Depth : register(u1);
RWTexture2D<float> Selection : register(u2);

cbuffer Parameters : register(b0)
{
    uint2 Size;
    float2 MotionScale;
    uint EdgeWidth;
    float InteriorStrength;
    uint DebugMode;
    uint Reserved;
};

uint2 packedAt(int2 pixel)
{
    if (any(pixel < 0) || any(pixel >= int2(Size)))
        return uint2(0, 0);
    return ObjectMotion.Load2((pixel.y * Size.x + pixel.x) * 8);
}

uint objectId(uint2 packed) { return packed.x & 0x7fffu; }

int signed11(uint value)
{
    value &= 0x7ffu;
    return (value & 0x400u) ? int(value) - 2048 : int(value);
}

bool objectBoundary(int2 pixel, uint id)
{
    [loop]
    for (int radius = 1; radius <= int(EdgeWidth); ++radius)
    {
        if (objectId(packedAt(pixel + int2(radius, 0))) != id ||
            objectId(packedAt(pixel - int2(radius, 0))) != id ||
            objectId(packedAt(pixel + int2(0, radius))) != id ||
            objectId(packedAt(pixel - int2(0, radius))) != id ||
            objectId(packedAt(pixel + int2(radius, radius))) != id ||
            objectId(packedAt(pixel + int2(radius, -radius))) != id ||
            objectId(packedAt(pixel + int2(-radius, radius))) != id ||
            objectId(packedAt(pixel - int2(radius, radius))) != id)
            return true;
    }
    return false;
}

[numthreads(8, 8, 1)]
void ApplyObjectMotion(uint3 dispatchId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchId.xy);
    if (any(dispatchId.xy >= Size))
        return;

    float4 originalMotion = Motion[pixel];
    float originalDepth = Depth[pixel];
    uint2 packed = packedAt(pixel);
    uint id = objectId(packed);
    if (!id)
    {
        Selection[pixel] = 0;
        return;
    }

    uint motionX = (packed.y >> 3) & 0x7ffu;
    uint motionY = ((packed.x >> 24) | (packed.y << 8)) & 0x7ffu;
    float2 objectPixels = float2(signed11(motionX), signed11(motionY)) * 0.125;
    float2 objectMotion = objectPixels / max(MotionScale, float2(1e-6, 1e-6));
    float opacity = float((packed.x >> 16) & 0xffu) / 255.0;
    bool edge = EdgeWidth != 0 && objectBoundary(pixel, id);
    float weight = edge ? 1.0 : saturate(opacity * InteriorStrength);

    originalMotion.xy = lerp(originalMotion.xy, objectMotion, weight);
    if (edge)
    {
        uint depthKey = packed.y >> 14;
        bool reverseDepth = (packed.x & 0x8000u) != 0;
        uint depthBits = reverseDepth ? depthKey : 0x3ffffu - depthKey;
        originalDepth = float(depthBits) / 262143.0;
    }
    Motion[pixel] = originalMotion;
    Depth[pixel] = originalDepth;
    Selection[pixel] = edge ? 1.0 : weight;
}
