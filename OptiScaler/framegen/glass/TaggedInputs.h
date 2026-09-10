#pragma once
#include <array>
#include <cstdint>

namespace GlassFg
{
// Scalar metadata only. The caller serializes observe/read/clear. Native
// resources remain owned by Streamline; never dereference these identities.
class TaggedInputs
{
  public:
    struct Tag
    {
        const void* resource = nullptr;
        uint64_t frame = UINT64_MAX;
        uint32_t viewport = 0, state = 0;
        uint32_t left = 0, top = 0, width = 0, height = 0;
        bool valid = false;
    };

  private:
    std::array<Tag, 3> tags {};
    std::array<uint64_t, 3> revisions {};
    uint64_t revision = 0, consumed = 0;
    const void* feature = nullptr;
    std::array<const void*, 3> batch {};
    unsigned next = 0, phases = 0;

  public:
    void clear() { *this = {}; }
    void observe(unsigned type, Tag tag)
    {
        if (type >= tags.size())
            return;
        tags[type] = tag;
        revisions[type] = ++revision;
    }

    // Streamline types are depth/motion/color; native inputs use motion/color/depth.
    // Only the observed clone COPY_DEST contract is admitted. A fresh complete
    // tag set is required for phase 1, even when the same resource is reused.
    bool read(const void* handle, unsigned index, unsigned count, const void* const (&resources)[3], Tag (&result)[3])
    {
        if (!handle || (count != 1 && count != 3) || !index || index > count)
        {
            next = 0;
            return false;
        }
        constexpr unsigned order[] = { 1, 2, 0 };
        if (index == 1)
        {
            next = 0;
            bool good = true;
            for (unsigned i = 0; i < 3; ++i)
            {
                const auto& t = tags[order[i]];
                good &= resources[i] && t.valid && t.resource == resources[i] && t.state == 0x400 && !t.viewport &&
                        !t.left && !t.top && t.frame != UINT64_MAX && t.frame == tags[0].frame &&
                        revisions[order[i]] > consumed;
            }
            consumed = revision; // A failed/incomplete phase cannot reuse older observations.
            if (!good)
                return false;
            feature = handle;
            phases = count;
            for (unsigned i = 0; i < 3; ++i)
                batch[i] = resources[i];
        }
        else if (feature != handle || phases != count || next != index)
        {
            next = 0;
            return false;
        }
        for (unsigned i = 0; i < 3; ++i)
        {
            // Reject changed tags/identities during a generated-frame batch.
            if (resources[i] != batch[i] || !tags[order[i]].valid || tags[order[i]].resource != batch[i] ||
                tags[order[i]].state != 0x400)
            {
                next = 0;
                return false;
            }
            result[i] = tags[order[i]];
        }
        next = index < count ? index + 1 : 0;
        return true;
    }
};
} // namespace GlassFg
