#pragma once
#include "PackedMotionCapture.h"

namespace GlassFg
{
// Product-side identity for the packed object-motion capture. It uses only data
// this module already observes: the revalidated draw batch spans, the object
// registry's registration lifetime serial, the bound depth target's resource
// identity and the validated pipeline entry. Grouped arrays additionally use
// the element list the module's own engine hooks publish (CyberpunkGroups ->
// GlassArrayMapping). No image estimation and no new motion math is introduced.
PackedMotionIdentityProvider MakeGlassMotionIdentityProvider() noexcept;

// Per-reason counters for the identity gate above. Diagnostics only.
struct GlassMotionIdentityStats
{
    std::uint64_t resolved = 0, rejected = 0, noOwner = 0, noView = 0, noLifetime = 0, noElementIndex = 0;
    // Split of the element-index rejection: an array packet whose parent
    // identity is missing cannot be indexed at all, while a parent that is
    // present but has no verified source order needs the engine's own order.
    std::uint64_t noElementParent = 0, noElementOrder = 0;
    // Split of noView: the command has no raster state yet, the raster state is
    // deliberately unknown (render pass, bundle or a >8 target call), or the
    // targets are known but their descriptors are not in the tracked view map.
    std::uint64_t noViewState = 0, noViewUnknown = 0, noViewDescriptor = 0;
    // Split of the descriptor rejection: the draw names no view record at all,
    // or it names records whose resource field is still zero. Both read as one
    // descriptor miss in the aggregate above.
    std::uint64_t noViewNoRecord = 0, noViewNullResource = 0;
    // Grouped array elements: the engine's own source index was published for
    // this pool ordinal, or the lookup missed and the draw ordinal was used.
    // The pair is what tells a live session whether the engine mapping reached
    // the identity key.
    std::uint64_t elementMapped = 0, elementUnmapped = 0;
};
GlassMotionIdentityStats ReadGlassMotionIdentityStats() noexcept;
}
