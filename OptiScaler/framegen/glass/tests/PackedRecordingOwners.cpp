#include "../PackedRecordingOwners.h"
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace
{
unsigned checks = 0;
void require(bool okay, const char* message)
{
    ++checks;
    if (!okay)
        throw std::runtime_error(message);
}
struct Input
{
    std::uint64_t frame;
    const void* motion;
    float jitter;
    bool sameRenderedFrame(const Input& other) const
    {
        return frame == other.frame && motion == other.motion && jitter == other.jitter;
    }
};
}
int main()
{
    try
    {
        int commands[17] {};
        GlassFg::PackedRecordingOwners owners;
        require(!owners.add(nullptr), "Null recording admitted");
        require(owners.empty(), "New owner set not empty");
        for (auto& command : commands)
        {
            const auto index = &command - commands;
            require(owners.add(&command) == (index < 16), "Capacity admission incorrect");
        }
        require(owners.add(&commands[3]), "Repeated evaluation consumed capacity");
        require(!owners.contains(&commands[16]), "Unrelated submission can consume compose");
        owners.discard(&commands[0]);
        require(!owners.contains(&commands[0]), "Discarded recording retained ownership");
        require(owners.contains(&commands[1]), "Discard removed another evaluation");
        require(owners.add(&commands[16]), "Discarded slot not reusable");
        owners.clear();
        require(owners.empty(), "Consumed frame retained recordings");

        Input tagged {900, &commands[0], 0.25f};
        Input untagged {UINT64_MAX, &commands[0], 0.25f};
        require(GlassFg::MatchUntaggedFrame(untagged, tagged, 100, 100), "Exact frame rejected");
        require(!GlassFg::MatchUntaggedFrame(untagged, tagged, 100, 101), "Previous MV accepted");
        require(!GlassFg::MatchUntaggedFrame(untagged, tagged, 100, 0), "Absent identity accepted");
        untagged.motion = &commands[1];
        require(!GlassFg::MatchUntaggedFrame(untagged, tagged, 100, 100), "Different guides accepted");
        untagged.motion = tagged.motion;
        untagged.jitter = -0.25f;
        require(!GlassFg::MatchUntaggedFrame(untagged, tagged, 100, 100), "Changed jitter accepted");

        GlassFg::PackedInlineKey key {&commands[0], &commands[1], &commands[2], &commands[3],
                                     100, 1920, 1080, 0.25f, -0.25f, 42};
        require(key == key, "Identical NR recording rejected");
        auto changed = key;
        changed.command = &commands[4];
        require(!(key == changed), "Unsubmitted other NR list reused");
        changed = key; ++changed.frame;
        require(!(key == changed), "Previous NR frame reused");
        changed = key; changed.jitterX = -0.25f;
        require(!(key == changed), "Changed NR jitter reused");
        changed = key; ++changed.controls;
        require(!(key == changed), "Changed opacity/settings reused");
        changed = key; changed.packed = &commands[5];
        require(!(key == changed), "Replaced packed slot reused");
        changed = key; changed.scaleY = std::numeric_limits<float>::quiet_NaN();
        require(!(key == changed), "Nonfinite NR key reused");
        std::printf("PACKED_RECORDING_OWNERS_OK checks=%u\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "PACKED_RECORDING_OWNERS_FAIL %s\n", error.what());
        return 1;
    }
}
