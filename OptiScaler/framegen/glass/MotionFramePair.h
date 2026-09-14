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
        if (!reset && engine && (engineFrame <= engine || fgFrame <= fg)) return false;
        const bool consecutive = !reset && engine && engine != UINT32_MAX &&
                                 engineFrame == engine + 1 && fg != UINT64_MAX && fgFrame == fg + 1;
        engine = engineFrame;
        fg = fgFrame;
        return consecutive;
    }
    std::uint32_t engineFrame() const { return engine; }
    std::uint64_t fgFrame() const { return fg; }
};
} // namespace GlassFg
