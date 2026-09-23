#pragma once
#include <array>
#include <cstdint>

namespace GlassFg
{
struct PackedInlineKey
{
    const void* command = nullptr;
    const void* packed = nullptr;
    const void* motion = nullptr;
    const void* depth = nullptr;
    std::uint32_t frame = 0;
    float scaleX = 0, scaleY = 0, jitterX = 0, jitterY = 0;
    std::uint64_t controls = 0;
    bool operator==(const PackedInlineKey&) const = default;
};

template <class Input>
bool MatchUntaggedFrame(const Input& inputs, const Input& batch, std::uint32_t packedFrame,
                        std::uint32_t engineFrame)
{
    auto identified = inputs;
    identified.frame = batch.frame;
    return engineFrame != 0 && packedFrame == engineFrame && identified.sameRenderedFrame(batch);
}

// A pending compose may serve several evaluations of one rendered frame.
// Only a submission containing one of those recordings may consume it.
class PackedRecordingOwners
{
    std::array<const void*, 16> owners {};

  public:
    bool contains(const void* command) const
    {
        if (!command)
            return false;
        for (auto* owner : owners)
            if (owner == command)
                return true;
        return false;
    }
    bool add(const void* command)
    {
        if (!command)
            return false;
        if (contains(command))
            return true;
        for (auto*& owner : owners)
            if (!owner)
            {
                owner = command;
                return true;
            }
        return false;
    }
    void discard(const void* command)
    {
        for (auto*& owner : owners)
            if (owner == command)
                owner = nullptr;
    }
    bool empty() const
    {
        for (auto* owner : owners)
            if (owner)
                return false;
        return true;
    }
    void clear() { owners = {}; }
};
} // namespace GlassFg
