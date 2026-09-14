#pragma once
#include <cstdint>

// Synchronous scalar lookup for replaceable diagnostics in the same process.
// The caller owns the live producer argument. This is not a lifetime lease,
// previous-frame guarantee, transform pointer, or permission to generate MV.
struct GlassExperimentSourceOwner
{
    std::uint32_t size = sizeof(GlassExperimentSourceOwner), version = 1;
    std::uint64_t node = 0, buffer = 0;
    std::uint32_t first = 0, count = 0, generation = 0, reserved = 0;
};
using GlassExperimentSourceQuery = std::int32_t (*)(std::uint64_t proxy, std::uint64_t mesh,
                                                  std::uint32_t originalCount,
                                                  GlassExperimentSourceOwner* output);

// Current recorded producer provenance only. Indices are relative to the
// renderer input array, not a persistent baked source identity. No MV admission.
struct GlassExperimentInstanceSource
{
    std::uint32_t size = sizeof(GlassExperimentInstanceSource), version = 1;
    std::uint64_t proxy = 0, mesh = 0, renderer = 0, scene = 0;
    std::uint32_t frame = 0, ownerSlot = 0, originalCount = 0, selectedCount = 0;
    std::uint32_t linear = 0, reserved = 0;
    std::uint16_t indices[64] {};
};
using GlassExperimentInstanceQuery = std::int32_t (*)(std::uint32_t frame, std::uint64_t mesh,
    std::uint32_t globalStart, std::uint32_t count, GlassExperimentInstanceSource* output);

// Additive diagnostic ABI. owner.generation==0 means source provenance is absent.
// No old result layout changes; neither version grants temporal validity.
struct GlassExperimentInstanceSourceV2
{
    std::uint32_t size = sizeof(GlassExperimentInstanceSourceV2), version = 2;
    GlassExperimentInstanceSource instance;
    GlassExperimentSourceOwner owner;
};
using GlassExperimentInstanceQueryV2 = std::int32_t (*)(std::uint32_t frame, std::uint64_t mesh,
    std::uint32_t globalStart, std::uint32_t count, GlassExperimentInstanceSourceV2* output);

// Borrowed directly from the currently executing native flush. A parent slot
// and input-array header do not establish child identity or previous motion.
struct GlassExperimentPacketParent
{
    std::uint32_t size = sizeof(GlassExperimentPacketParent), version = 1;
    std::uint64_t proxy = 0, mesh = 0, entry = 0, sourceArray = 0;
    std::uint32_t slot = 0, arrayCount = 0, arrayGlobalStart = UINT32_MAX;
    std::uint32_t first = 0, count = 0, transformIndex = 0, globalRange = 0, reserved = 0;
};
using GlassExperimentPacketQuery = std::int32_t (*)(std::uint64_t callsite, std::uint64_t mesh,
    std::uint32_t chunk, std::uint32_t instances, std::uint32_t ordinal,
    std::uint32_t first, std::uint32_t count, std::uint32_t transformIndex,
    std::uint32_t globalRange, GlassExperimentPacketParent* output);

// Same-flush original flags; permits distinguishing grouped selection from
// linear storage. Does not establish previous transforms or persistent identity.
struct GlassExperimentPacketParentV2
{
    std::uint32_t size = sizeof(GlassExperimentPacketParentV2), version = 2;
    GlassExperimentPacketParent parent;
    std::uint32_t instanceFlags = 0, flagsValid = 0;
};
using GlassExperimentPacketQueryV2 = std::int32_t (*)(std::uint64_t, std::uint64_t,
    std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
    std::uint32_t, std::uint32_t, GlassExperimentPacketParentV2*);
