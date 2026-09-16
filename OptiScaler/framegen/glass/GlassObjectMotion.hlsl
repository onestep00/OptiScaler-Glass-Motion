// The packed object records are written by the capture raster as a UAV, so the
// compose reads them as a UAV too: one state for one resource, and the UAV
// barrier between the capture writes and this read is well defined.
RWByteAddressBuffer ObjectMotion : register(u0);
RWTexture2D<float4> Motion : register(u1);
RWTexture2D<float> Depth : register(u2);
RWTexture2D<float> Selection : register(u3);
RWByteAddressBuffer Counters : register(u4);

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
    // Diagnostic bit 2: exercise the dispatch without reading the packed
    // records. A stall that survives this proves the read is not the cause.
    if (DebugMode & 2u)
    {
        Motion[pixel] = originalMotion;
        Depth[pixel] = originalDepth;
        Selection[pixel] = 0.0;
        return;
    }
    uint2 packed = packedAt(pixel);
    uint id = objectId(packed);
    uint ignored = 0;
    bool count = DebugMode != 0;
    if (count)
        Counters.InterlockedAdd(0, 1, ignored);
    if (!id)
    {
        Selection[pixel] = 0;
        return;
    }
    if (count)
        Counters.InterlockedAdd(4, 1, ignored);

    uint motionX = (packed.y >> 3) & 0x7ffu;
    uint motionY = ((packed.x >> 24) | (packed.y << 8)) & 0x7ffu;
    float2 objectPixels = float2(signed11(motionX), signed11(motionY)) * 0.125;
    // MotionScale is the provider's texel->pixel factor (DLSSG.MvecScaleX/Y),
    // 2560x1440 in the live path: pixels = texel * MotionScale. The engine
    // stores normalized motion here (dump 2026-09-16: |original| <= 0.0161
    // while a pixel-space frame would show tens of pixels on any moving
    // content), and packed records are captured in pixels, so the texel value
    // is pixels / MotionScale. The earlier expression multiplied by
    // MotionScale and divided by Size, which cancelled to pixels and left
    // every covered value ~2560x above the engine's own range.
    float2 objectMotion = objectPixels / MotionScale;
    float opacity = float((packed.x >> 16) & 0xffu) / 255.0;
    bool edge = EdgeWidth != 0 && objectBoundary(pixel, id);
    // The visible boundary always takes the object's own motion. Inside it a
    // fully transmissive material reports no opacity, so scaling opacity alone
    // left the whole object body on the engine's motion of whatever was drawn
    // behind it: a glass in front of moving content travelled with that content
    // instead of with the surface. InteriorStrength is the fraction of such a
    // transmissive pixel that still follows the surface itself (0 = keep the
    // content behind it, 1 = every packed pixel follows the surface).
    float weight = edge ? 1.0 : saturate(opacity + (1.0 - opacity) * InteriorStrength);
    if (count)
    {
        if (edge)
            Counters.InterlockedAdd(8, 1, ignored);
        else if (weight > 0.0)
            Counters.InterlockedAdd(12, 1, ignored);
    }

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
