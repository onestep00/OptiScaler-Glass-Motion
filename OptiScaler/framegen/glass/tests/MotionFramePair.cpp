#include "../MotionFramePair.h"
#include <cstdio>
#include <stdexcept>
static void check(bool value) { if (!value) throw std::runtime_error("frame pair contract failed"); }
int main()
{
    try
    {
        GlassFg::MotionFramePair pair;
        check(!pair.advance(0, 9));              // Engine frame zero is invalid.
        check(!pair.advance(500, UINT64_MAX));   // FG sentinel is invalid.
        check(pair.advance(500, 0));             // First pair accepted; FG zero is valid.
        check(!pair.advance(500, 1));            // Equal engine mark is stale.
        check(!pair.advance(499, 2));            // Backward engine mark is stale.
        check(!pair.advance(501, 0));            // Equal FG mark is stale.
        check(pair.advance(501, 1));             // Forward pair accepted.
        check(pair.advance(504, 8));             // Separate domains: forward gaps are accepted.
        check(pair.advance(505, 9));
        check(pair.engineFrame() == 505 && pair.fgFrame() == 9);
        check(!pair.advance(505, 10));           // Equal engine mark after a gap.
        check(!pair.advance(504, 10));           // Backward engine mark after a gap.
        check(!pair.advance(20, 0));             // Implicit counter reset rejected.
        check(!pair.advance(0, 0, true));        // Zero engine frame stays invalid on reset.
        check(pair.advance(20, 0, true));        // Explicit reset accepts the new pair.
        check(!pair.advance(20, 0));             // Equal marks after reset rejected.
        check(pair.advance(21, 1));
        check(!pair.advance(UINT32_MAX, 10, true)); // Sentinel engine frame rejected.
        check(!pair.advance(1, 11));                // No implicit modular wrap.
        check(!pair.advance(101, UINT64_MAX));
        puts("MOTION_FRAME_PAIR independent_domains=pass forward_pairs_only=pass reset_gap_wrap=pass");
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
