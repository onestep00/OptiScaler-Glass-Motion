#pragma once
#include "GeometrySourceOwners.h"
#include "GeometrySourceSlots.h"

namespace GlassFg
{
// Diagnostic candidate from an observed creation, not a persisted engine ID.
struct CyberpunkSourceCandidate
{
    std::uint64_t node, definition, nodeControl, shared, sharedControl, handle, proxy, meshAsset;
    std::uint32_t first, count, ordinal, reserved;
};
static_assert(sizeof(CyberpunkSourceCandidate) == 80);
struct CyberpunkSourceSnapshot
{
    GeometrySourceOwners<1>::Source source;
    std::uint64_t handle = 0, proxy = 0, handleControl = 0, transforms = 0;
    bool operator==(const CyberpunkSourceSnapshot&) const = default;
};
template <typename Read>
bool ReadCyberpunkSourceCandidate(const CyberpunkSourceCandidate& candidate,
                                  CyberpunkSourceSnapshot& result, Read read)
{
    result = {};
    const auto at = [&](std::uint64_t base, unsigned offset, auto& value)
    {
        return base >= 0x10000 && base <= 0x7fffffffffffULL - offset - sizeof(value) &&
               read(base + offset, &value, sizeof(value));
    };
    if (candidate.reserved || !candidate.count || candidate.ordinal >= 4096) return false;
    std::array<std::uint64_t, 2> nodeHeader {}, array {}, entry {};
    std::array<std::uint64_t, 3> range {};
    std::uint64_t currentMesh = 0, renderMesh = 0, proxyMesh = 0, proxy = 0, data = 0, transforms = 0;
    std::uint32_t bytes = 0, count = 0;
    if (!at(candidate.node, 0x60, nodeHeader) || nodeHeader[0] != candidate.definition || nodeHeader[1] != candidate.nodeControl ||
        !at(nodeHeader[0], 0x38, range) || range[0] != candidate.shared || range[1] != candidate.sharedControl ||
        !at(candidate.node, 0xb8, currentMesh) || currentMesh != candidate.meshAsset || !at(currentMesh, 0x1f0, renderMesh) ||
        !at(candidate.node, 0xe8, array)) return false;
    const auto capacity = static_cast<std::uint32_t>(array[1]), size = static_cast<std::uint32_t>(array[1] >> 32);
    if (size > capacity || size > 4096 || candidate.ordinal >= size ||
        !at(array[0], candidate.ordinal * 16, entry) || entry[0] != candidate.handle ||
        !at(entry[0], 0x10, proxy) || proxy != candidate.proxy ||
        !at(proxy, 0xd8, proxyMesh) || !renderMesh || proxyMesh != renderMesh ||
        !at(proxy, 0x108, transforms) || !at(proxy, 0x110, count) || count != candidate.count ||
        !at(range[0], 0x30, data) || !at(range[0], 0x38, bytes)) return false;
    const auto startBytes = std::uint64_t(candidate.first) * 48;
    const auto endBytes = (std::uint64_t(candidate.first) + candidate.count) * 48;
    if (data > UINT64_MAX - endBytes) return false;
    const auto sourceSpan = GeometrySourceSpan::resolve(data, bytes, static_cast<std::uint32_t>(range[2]),
        static_cast<std::uint32_t>(range[2] >> 32), data + startBytes, data + endBytes, 48);
    if (!sourceSpan || sourceSpan.first != candidate.first || sourceSpan.count != candidate.count) return false;
    // The transient producer pointer can be null outside its update. This is
    // owner metadata only; it never admits a transform read or motion history.
    result = {{candidate.node, candidate.shared, renderMesh, sourceSpan.first, sourceSpan.count},
              entry[0], proxy, entry[1], transforms};
    return true;
}
} // namespace GlassFg
