cbuffer DispatchConstants : register(b0)
{
    uint candidateCount;
    uint pixelCount;
    uint2 unused;
};

RWByteAddressBuffer packedMotion : register(u0);

uint64_t packCandidate(uint depth, int motionX, int motionY, uint weight, uint objectId)
{
    return ((uint64_t(depth) & 0xfffff) << 44) |
           ((uint64_t(uint(motionX) & 0xfff)) << 32) |
           ((uint64_t(uint(motionY) & 0xfff)) << 20) |
           ((uint64_t(weight & 0xff)) << 12) |
           uint64_t(objectId & 0xfff);
}

[numthreads(8, 1, 1)]
void main(uint3 threadId : SV_DispatchThreadID)
{
    const uint candidate = threadId.x / pixelCount;
    const uint pixel = threadId.x % pixelCount;
    if (candidate >= candidateCount)
        return;

    // Every layer competes for the same screen pixel. The high depth key makes
    // the nearest layer win while its motion, weight and ID remain inseparable.
    const uint depth = 1 + ((candidate * 37 + pixel * 11) % 97);
    const int motionX = int(candidate * 23 + pixel * 7) - 96;
    const int motionY = 80 - int(candidate * 19 + pixel * 5);
    const uint weight = candidate * 29 + pixel * 3;
    const uint objectId = candidate * 101 + pixel * 17 + 1;
    uint64_t previous;
    packedMotion.InterlockedMax64(pixel * 8, packCandidate(depth, motionX, motionY, weight, objectId), previous);
}
