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
    // 0..1. A covered pixel takes the object's motion and depth when the
    // material opacity of that pixel reaches this value. Pixels below it keep
    // the engine's value byte for byte.
    float OpacityThreshold;
    uint DebugMode;
    // The delivery converts the captured object motion from the frame's
    // jittered projection to the engine's motion convention by removing the
    // frame-global projection step between the captured frame and its
    // predecessor. Jitter is the provider's declared offset of the captured
    // frame, PreviousJitter the value the previous compose consumed. The two
    // are equal while no predecessor exists, which makes the term zero on the
    // first delivered frame. The term reaches only the pixels this shader
    // replaces; the engine's own motion is never read or written by it.
    uint JitterMode;
    float2 Jitter;
    float2 PreviousJitter;
    float JitterGain;
    // Engine-proximity gate radius in pixels. A pixel the coverage rule would
    // take is left to the engine when the object's own motion is already within
    // this distance of the engine's value: the injection would only re-quantize
    // an already correct surface onto the 1/8 px record grid, and the
    // 2026-09-19 Songbird mirror band A/B measured that sub-quantum bias as
    // extra screen temporal variance against `substitute=off`. Diagnostic bit 3
    // (DebugMode & 8) disables the gate. Keeps the constant block at 56 bytes.
    float GatePx;
    // Diagnostic bit 4 (DebugMode & 16): substitute the object's motion but
    // leave the engine's depth under it. The engine's transparent pass writes
    // no depth, so the value there belongs to the content behind the surface;
    // keeping it makes the generator resolve occlusion against that content
    // while the motion still follows the surface. The bit shares the existing
    // word, so the block stays 56 bytes.
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
    // Diagnostic bit 4: deliver an all-zero motion field to the generator. The
    // FG then has no motion at all, which is the baseline an artifact report
    // needs. Depth keeps the engine's value and the game's own motion texture
    // is never written.
    if (DebugMode & 4u)
    {
        Motion[pixel] = 0.0;
        Depth[pixel] = originalDepth;
        Selection[pixel] = 0.0;
        return;
    }
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
    // Only diagnostic requests enable counters. Gate-off is the normal
    // delivery policy and must not enable pixel-global atomic diagnostics.
    // Other delivery mode bits likewise do not request measurement.
    bool count = (DebugMode & 7u) != 0;
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
    // The engine feeds normalized motion: its own MV.Scale parameter equals the
    // render extent (2560x1440 in the live path), so pixels = texel * extent and
    // the injected texel is pixels / extent. MotionScale is the provider's
    // declaration and is 1 in the Streamline driver path even though the texture
    // stays normalized there; dividing by 1 left every covered value 2560x above
    // the engine's range (dump 2026-09-16 11:38: covered |mv| median 905px vs
    // engine 0.48px). Only trust the declared scale when it is a real
    // texel->pixel factor (> 1.5), otherwise convert with the render extent.
    float2 mvUnit = any(MotionScale > 1.5) ? MotionScale : float2(Size);
    float2 objectMotion = objectPixels / mvUnit;
    // Delivery rule (2026-09-17): the value stays the object's own motion. No
    // engine value is blended in and no strength scales it. The capture
    // computes the motion in the frame's jittered projection; the capture
    // endpoint already adds that convention's frame-to-frame constant, so the
    // default mode here is 0 and the epsilon-free term below is a diagnostic.
    // Mode 3 measured 2026-09-19 on the delta build re-adds +0.887*J to the
    // delivered field and raises the boundary step from 0.023 to 0.348 px.
    float2 jitter = float2(0.0, 0.0);
    if (JitterMode == 1u)
        jitter = -PreviousJitter;
    else if (JitterMode == 2u)
        jitter = PreviousJitter;
    else if (JitterMode == 3u)
        jitter = Jitter;
    else if (JitterMode == 4u)
        jitter = -Jitter;
    else if (JitterMode == 5u)
        jitter = Jitter - PreviousJitter;
    objectMotion += (jitter * JitterGain) / mvUnit;
    float opacity = float((packed.x >> 16) & 0xffu) / 255.0;
    // Coverage class. The capture sets the top bit of the 18-bit depth key when
    // the pixel's material opacity reached the threshold. The packed store is an
    // unsigned max, so covered records always outrank uncovered ones and the
    // nearest surface wins inside each class. One 8-byte record per pixel
    // therefore resolves any number of overlapping transparent layers: the
    // nearest surface the border/opacity rule keeps wins, and a nearly
    // transparent layer no longer hides a visible one behind it.
    const uint depthKey18 = packed.y >> 14;
    const bool covered = (depthKey18 & 0x20000u) != 0;
    const bool edge = EdgeWidth != 0 && objectBoundary(pixel, id);
    // Delivery rule (2026-09-17): a pixel either takes the object's own motion
    // and depth exactly, or it keeps the engine's value byte for byte. The
    // previous form blended the object motion into the covered pixel by
    // opacity * strength, so every covered pixel moved by a fraction of
    // surface-minus-background and the region swung between the two while the
    // camera moved, which read as a ripple. There is no strength, no blend and
    // no interior clip now.
    // The visible boundary always takes the exact object motion, whatever its
    // opacity. The interior takes it when the material opacity reaches the
    // threshold; below it the pixel belongs to the content behind the surface
    // and is left alone. Thin low-opacity features (particle sprites, thin
    // glass edges) are covered by this rule too, because they are part of the
    // scene and have to move with their own motion instead of the background.
    const bool apply = covered || edge;
    if (count)
    {
        if (apply)
            Counters.InterlockedAdd(edge ? 8u : 12u, 1, ignored);
        // Opacity histogram of the packed pixels (<0.25, <0.5, <0.75, >=0.75)
        // and the outcome per packed pixel: not covered, boundary band applied
        // below the interior threshold, boundary take, interior take. The sum
        // of the four outcome buckets is the packed pixel count; the dump names
        // them apply_buckets.
        const uint opacityBucket = opacity < 0.25 ? 16u : opacity < 0.5 ? 20u : opacity < 0.75 ? 24u : 28u;
        Counters.InterlockedAdd(opacityBucket, 1, ignored);
        const uint outcomeBucket = !apply ? 32u : (edge ? (covered ? 40u : 36u) : 44u);
        Counters.InterlockedAdd(outcomeBucket, 1, ignored);
    }
    if (!apply)
    {
        Selection[pixel] = 0.0;
        return;
    }
    // Simultaneous stripe A/B (diagnostic bit 5, live channel only). Even
    // 64-pixel column bands take the object's motion and depth, odd bands keep
    // the engine's values byte for byte, so one delivered frame carries both
    // arms under the same pan and the same animation phase. The band width is a
    // compile-time constant so the 56-byte block stays unchanged.
    // The odd bands are counted separately (slot 14, byte offset 56) because
    // edge_pixels and interior_pixels above count the coverage rule's selection:
    // the coverage document defines their sum as the packed pixel count, and the
    // stripe only decides which of those selections reach the delivered frame.
    // `corrected = edge + interior - stripe_skip` is therefore the exact count of
    // pixels this dispatch wrote, and stripe_skip stays zero on every other path
    // so no existing capture changes meaning.
    if ((DebugMode & 32u) != 0u && ((uint(pixel.x) / 64u) & 1u) != 0u)
    {
        if (count)
            Counters.InterlockedAdd(56u, 1u, ignored);
        Selection[pixel] = 0.0;
        return;
    }
    // Engine-proximity gate (diagnostic bit 3 disables it). Only pixels the
    // coverage rule already takes reach this test, so the gate costs one
    // compare and one subtract on the substituted set. The engine's own value
    // is never modified: a gated pixel keeps its motion and depth byte for
    // byte, exactly like an uncovered one.
    if ((DebugMode & 8u) == 0u)
    {
        float2 gateDeltaPx = abs(objectMotion - originalMotion.xy) * mvUnit;
        if (all(gateDeltaPx < GatePx))
        {
            if (count)
                Counters.InterlockedAdd(48u, 1u, ignored);
            Selection[pixel] = 0.0;
            return;
        }
    }
    // Motion and depth have to describe the same surface. The engine writes no
    // depth for the transparent pass, so the depth under a corrected motion
    // still belongs to the content behind the object; a generator that checks
    // one against the other keeps the background flow for that region. Both
    // values come from the object's own record.
    // The top bit of the high key is the coverage class, not depth.
    uint depthKey = depthKey18 & 0x1ffffu;
    bool reverseDepth = (packed.x & 0x8000u) != 0;
    uint depthBits = reverseDepth ? depthKey : 0x1ffffu - depthKey;
    float objectDepth = float(depthBits) / 131071.0;
    // Diagnostic bit 4: keep the engine's depth under a substituted motion. The
    // dump header's depth_sub counts the pixels that did take the object depth,
    // so a capture names the route it measured: depth_sub is the applied count
    // while the bit is off, and zero while it is on.
    const bool keepDepth = (DebugMode & 16u) != 0;
    if (count && !keepDepth)
        Counters.InterlockedAdd(52u, 1u, ignored);
    Motion[pixel] = float4(objectMotion, originalMotion.z, originalMotion.w);
    Depth[pixel] = keepDepth ? originalDepth : objectDepth;
    Selection[pixel] = 1.0;
}
