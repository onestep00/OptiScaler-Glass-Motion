#pragma once
#include "PackedMotionCapture.h"

namespace GlassFg
{
// Product-side identity for the packed object-motion capture. It uses only data
// this module already observes: the revalidated draw batch spans, the object
// registry's registration lifetime serial, the bound depth target's resource
// identity and the validated pipeline entry. No engine hook or engine memory
// read is added, and no new motion math is introduced.
PackedMotionIdentityProvider MakeGlassMotionIdentityProvider() noexcept;

// Per-reason counters for the identity gate above. Diagnostics only.
struct GlassMotionIdentityStats
{
    std::uint64_t resolved = 0, rejected = 0, noOwner = 0, noView = 0, noLifetime = 0, noElementIndex = 0;
};
GlassMotionIdentityStats ReadGlassMotionIdentityStats() noexcept;
}
