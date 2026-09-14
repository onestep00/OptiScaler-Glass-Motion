#include "../MotionFramePair.h"
#include <cstdio>
#include <stdexcept>
static void check(bool value) { if (!value) throw std::runtime_error("frame pair contract failed"); }
int main()
{
    GlassFg::MotionFramePair pair;
    check(!pair.advance(0, 9));
    check(!pair.advance(500, UINT64_MAX));
    check(!pair.advance(500, 0)); // Different domains, and FG frame zero is valid.
    check(pair.advance(501, 1));
    check(!pair.advance(501, 2));
    check(pair.advance(502, 2));
    check(!pair.advance(503, 2));
    check(pair.advance(503, 3));
    check(!pair.advance(504, 8)); // Skipped FG frames invalidate that MV pair.
    check(pair.advance(505, 9));
    check(!pair.advance(510, 10)); // Skipped engine frames invalidate independently.
    check(pair.advance(511, 11));
    check(!pair.advance(20, 0)); // Reload/counter reset must be explicit.
    check(pair.engineFrame() == 511 && pair.fgFrame() == 11);
    check(!pair.advance(20, 0, true));
    check(pair.advance(21, 1));
    check(!pair.advance(UINT32_MAX, 10, true));
    check(!pair.advance(1, 11)); // No implicit modular wrap.
    check(!pair.advance(100, UINT64_MAX - 1, true));
    check(!pair.advance(101, UINT64_MAX));
    puts("MOTION_FRAME_PAIR independent_domains=pass exact_n_minus_1=pass reset_gap_wrap=pass");
}
