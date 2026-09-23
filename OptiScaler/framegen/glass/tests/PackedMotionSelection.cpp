#include "../PackedMotionSelection.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace
{
struct Frame
{
    std::uint32_t number;
    std::uint64_t producerValue;
    bool clearSubmitted;
    const void* consumerCommand;
};
unsigned checks = 0;
void require(bool result, const char* message)
{
    ++checks;
    if (!result)
        throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        using GlassFg::SelectSecondConsumerFrame;
        std::array<Frame, 3> frames {{{100, 10, true, nullptr}, {101, 11, true, nullptr}, {102, 12, true, nullptr}}};
        // Slot order, producer age and a newer completed frame cannot change
        // the requested identity. Exercise every ring permutation.
        do
        {
            auto* selected = SelectSecondConsumerFrame(frames, 101);
            require(selected && selected->number == 101, "Exact current frame was not selected");
        } while (std::next_permutation(frames.begin(), frames.end(),
                                      [](const Frame& a, const Frame& b) { return a.number < b.number; }));
        require(!SelectSecondConsumerFrame(frames, 99), "Future producer selected for missing frame");
        require(!SelectSecondConsumerFrame(frames, 103), "Previous producer selected for missing frame");
        require(!SelectSecondConsumerFrame(frames, 0), "Unknown frame selected newest producer");
        require(!SelectSecondConsumerFrame(frames, UINT32_MAX), "Reserved frame admitted");
        require(!SelectSecondConsumerFrame(frames, UINT64_MAX), "Missing frame admitted");
        require(!SelectSecondConsumerFrame(frames, (std::uint64_t{1} << 32) + 101), "Frame truncated to 32 bits");
        frames[1].producerValue = 0;
        require(!SelectSecondConsumerFrame(frames, 101), "Unsubmitted frame fell back to another frame");
        frames[1].producerValue = 11;
        frames[1].clearSubmitted = false;
        require(!SelectSecondConsumerFrame(frames, 101), "Uncleared frame admitted");
        frames[1].clearSubmitted = true;
        frames[1].consumerCommand = &frames;
        require(!SelectSecondConsumerFrame(frames, 101), "FG-owned frame admitted or replaced");
        frames[1].consumerCommand = nullptr;
        frames[2].number = 101;
        require(!SelectSecondConsumerFrame(frames, 101), "Ambiguous same-frame producers admitted");
        frames[2].number = 102;
        require(SelectSecondConsumerFrame(frames, 101) == &frames[1], "Valid route did not recover");
        std::printf("PACKED_SELECTION_OK checks=%u\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
