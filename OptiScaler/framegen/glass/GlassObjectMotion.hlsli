// Geometry correspondence only. The caller supplies matched current/previous
// deformed vertices and unjittered object/camera transforms for the SAME object.
// This does not acquire engine history or choose among overlapping surfaces.
#ifndef GLASS_OBJECT_MOTION_INCLUDED
#define GLASS_OBJECT_MOTION_INCLUDED

float4 GlassObjectClip(float3 localPosition, row_major float4x4 objectToWorld,
                       row_major float4x4 worldToClip)
{
    return mul(mul(float4(localPosition, 1), objectToWorld), worldToClip);
}

bool GlassObjectMotion(float4 currentClip, float4 previousClip, out float2 motionUV)
{
    motionUV = 0;
    if (!all(isfinite(currentClip)) || !all(isfinite(previousClip)) ||
        currentClip.w <= 1e-6 || previousClip.w <= 1e-6)
        return false;
    float2 currentUV = currentClip.xy / currentClip.w * float2(.5, -.5) + .5;
    float2 previousUV = previousClip.xy / previousClip.w * float2(.5, -.5) + .5;
    motionUV = previousUV - currentUV;
    // A previous point outside the viewport still has a valid geometric MV.
    // Visibility/history sampling must be checked separately by the caller.
    return all(isfinite(motionUV));
}

float GlassMotionWeight(float opacity, float objectEdge, float gain, float bias)
{
    float interior = saturate(opacity * gain + bias);
    // A confirmed edge selects this object's full geometric motion even at
    // low opacity. Visibility/identity/history admission belongs to the caller.
    return lerp(interior, 1.0, saturate(objectEdge));
}

#endif
