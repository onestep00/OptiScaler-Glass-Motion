#pragma once
#include <array>
#include <cstdint>

namespace GlassFg
{
// The NR caller names an engine frame, not a Streamline token. Submission or
// resource recency cannot substitute for that identity. Keep this selection
// independent of D3D12 so missing and ambiguous producer sets can be tested.
template <class Frame, std::size_t Count>
Frame* SelectSecondConsumerFrame(std::array<Frame, Count>& frames, std::uint64_t engineFrame) noexcept
{
    if (engineFrame == 0 || engineFrame >= UINT32_MAX)
        return nullptr;
    Frame* selected = nullptr;
    for (auto& frame : frames)
    {
        if (frame.number != engineFrame || !frame.producerValue || !frame.clearSubmitted || frame.consumerCommand)
            continue;
        if (selected)
            return nullptr;
        selected = &frame;
    }
    return selected;
}
} // namespace GlassFg
