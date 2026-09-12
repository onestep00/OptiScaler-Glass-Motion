#include "../GeometrySourceIndexMap.h"
#include "../CyberpunkInstanceSelection.h"
#include "../GeometrySourceSlots.h"
#include "../VertexHistoryCache.h"
#include <cstdio>
#include <cstring>

int main()
{
    GlassFg::GeometrySourceIndexMap<> map;
    std::array<std::uint64_t, 4> active {0x49, 0, 0, 0};
    if (!map.compact(100, 8, active) || map.packedCount() != 3) return 1;
    // The renderer subsequently selects packed slots [2,0,1], not baked IDs.
    std::array<std::uint16_t, 3> selected {2, 0, 1};
    GlassFg::CyberpunkInstanceSelection selection;
    selection.sourceIndices = reinterpret_cast<std::uint64_t>(selected.data());
    selection.count = selection.sourceCount = 3;
    unsigned reads = 0;
    auto read = [&](std::uint64_t address, void* out, unsigned size) {
        if (address < selection.sourceIndices || size != 2 || address - selection.sourceIndices > 4) return false;
        memcpy(out, reinterpret_cast<void*>(address), size); ++reads; return true;
    };
    const unsigned expected[] = {106, 100, 103};
    // Compose the actual map/selection with the existing history allocator.
    // Lifetimes and frame numbers here are fixture inputs, not native evidence.
    GlassFg::GeometrySourceSlots<8> slots;
    GlassFg::VertexHistoryCache<8, 4, 32, 4> history;
    GlassFg::VertexHistoryKey owner {{0x10000, 0x20000, 23, 1}, 9, 10, 11, 0, 4};
    auto ticket = slots.begin(owner.view, 100);
    for (unsigned i = 0; i < 3; ++i)
    {
        unsigned source = 0;
        if (!map.selectedIndex(selection, i, source, read) || source != expected[i]) return 2;
        if (!slots.publish(ticket, i, {owner.object.proxy, owner.object.mesh, 1, 1, source})) return 12;
    }
    if (!slots.seal(ticket) || !history.beginFrame(100, 0)) return 13;
    const auto previous106 = history.acquire(slots.resolveHistoryKey(ticket, 0, owner, 100), 6, 100);
    const auto previous100 = history.acquire(slots.resolveHistoryKey(ticket, 1, owner, 100), 6, 100);
    const auto previous103 = history.acquire(slots.resolveHistoryKey(ticket, 2, owner, 100), 6, 100);
    if (!previous106 || !previous100 || !previous103) return 14;
    active[0] = 0x4a; // Same population size, changed source membership.
    if (!map.compact(100, 8, active)) return 3;
    unsigned source = 0;
    if (!map.selectedIndex(selection, 1, source, read) || source != 101 || reads != 4) return 4;
    ticket = slots.begin(owner.view, 101);
    // Resolve all surviving/new members after changing both packing and draw order.
    selected = {1, 2, 0};
    for (unsigned i = 0; i < 3; ++i)
    {
        if (!map.selectedIndex(selection, i, source, read) ||
            !slots.publish(ticket, i, {owner.object.proxy, owner.object.mesh, 1, 1, source})) return 15;
    }
    if (!slots.seal(ticket) || !history.beginFrame(101, 100)) return 16;
    const auto current103 = history.acquire(slots.resolveHistoryKey(ticket, 0, owner, 101), 6, 101);
    const auto current106 = history.acquire(slots.resolveHistoryKey(ticket, 1, owner, 101), 6, 101);
    const auto current101 = history.acquire(slots.resolveHistoryKey(ticket, 2, owner, 101), 6, 101);
    if (!current103 || !current106 || !current101 ||
        current103.base != previous103.base || current103.generation != previous103.generation ||
        current106.base != previous106.base || current106.generation != previous106.generation ||
        current101.base == previous100.base || current101.generation == previous100.generation) return 17;
    const auto readsBeforeMismatch = reads;
    selection.sourceCount = 4;
    if (map.selectedIndex(selection, 0, source, read) || source != UINT32_MAX || reads != readsBeforeMismatch) return 5;
    if (!map.linear(400, 70000) || !map.originalIndex(69999, source) || source != 70399) return 6;
    if (map.linear(UINT32_MAX, 2) || map.originalIndex(0, source)) return 7;
    active = {}; active[0] = 1; active[1] = 1; active[3] = std::uint64_t {1} << 63;
    if (!map.compact(0, 256, active) || !map.originalIndex(2, source) || source != 255) return 8;
    if (!map.compact(0, 65, active) || map.packedCount() != 2) return 9;
    if (map.compact(0, 257, active) || map.originalIndex(0, source)) return 10;
    active = {};
    if (map.compact(0, 8, active) || map.originalIndex(0, source)) return 11;
    std::printf("PASS source_layout_to_renderer_selection=1 same_count_replacement=1 "
                "source_count_mismatch_rejected=1 stale_map_rejected=1 "
                "survivor_history_preserved=1 replacement_history_separated=1 map_bytes=%zu game_hooks=0\n", sizeof(map));
}
