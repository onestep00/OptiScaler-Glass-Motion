#pragma once
#include <cstdint>

namespace GlassFg
{
// Checks continuity of an ALREADY correlated engine/FG pair. It does not infer
// the pairing, assume equal counters, or authorize a producer from dimensions.
// Missing, reset and skipped frames bypass once; repeated/backward values may
// not move the high-water marks backward without an explicit reset.
class MotionFramePair
{
    std::uint32_t engine = 0;
    std::uint64_t fg = UINT64_MAX;
  public:
    bool advance(std::uint32_t engineFrame, std::uint64_t fgFrame, bool reset = false)
    {
        if (!engineFrame || fgFrame == UINT64_MAX) return false;
        // Only stale or backward pairs are rejected. A forward pair may skip
        // counters (engine draw frames and generated frames do not advance in
        // lockstep in every scene); the submission-order rule already proves
        // the producer precedes this FG call.
        if (!reset && engine && (engineFrame <= engine || fgFrame <= fg)) return false;
        engine = engineFrame;
        fg = fgFrame;
        return true;
    }
    std::uint32_t engineFrame() const { return engine; }
    std::uint64_t fgFrame() const { return fg; }
};
} // namespace GlassFg
