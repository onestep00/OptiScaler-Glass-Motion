#include "../CyberpunkInstanceSelection.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

using GlassFg::CyberpunkInstanceSelection;
void require(bool value) { if (!value) throw std::runtime_error("Instance selection failed"); }
int main()
{
    std::array<unsigned char, 0x58> group {};
    std::array<std::uint16_t, 40> indices {};
    const auto address = reinterpret_cast<std::uint64_t>(group.data());
    const auto source = reinterpret_cast<std::uint64_t>(indices.data());
    unsigned calls = 0, bytes = 0;
    const auto read = [&](std::uint64_t from, void* to, unsigned length)
    {
        ++calls; bytes += length;
        if (!((from >= address && from - address <= group.size() && length <= group.size() - (from - address)) ||
              (from >= source && from - source <= sizeof(indices) && length <= sizeof(indices) - (from - source))))
            return false;
        memcpy(to, reinterpret_cast<const void*>(from), length); return true;
    };
    const auto set = [&](unsigned length, unsigned global)
    {
        memcpy(group.data() + 0x18, &source, 8);
        memcpy(group.data() + 0x28, &length, 4);
        memcpy(group.data() + 0x50, &global, 4);
    };
    CyberpunkInstanceSelection view;
    CyberpunkInstanceSelection::Descriptor descriptor { 20160, 40, 0, 0, 0x10000, 0x10000 + 40 * 48 };
    for (unsigned i = 0; i < indices.size(); ++i) indices[i] = std::uint16_t(39 - i);
    set(40, 20160);
    require(view.resolve(address, descriptor, 40, read));
    require(calls == 3 && bytes == 16);
    for (unsigned i = 0; i < indices.size(); ++i)
    {
        unsigned actual = 0;
        require(view.originalIndex(i, actual, read) && actual == 39 - i);
    }
    require(calls == 43 && bytes == 96);
    // Next view contains one original instance. Ordinal zero is still source 17.
    indices[0] = 17; set(1, 30104);
    descriptor = { 30104, 1, 7, 0, 0x10000, 0x10000 + 40 * 48 };
    require(view.resolve(address, descriptor, 40, read));
    unsigned actual = 0;
    require(view.originalIndex(0, actual, read) && actual == 17);
    require(!view.originalIndex(1, actual, read) && actual == UINT32_MAX);
    indices[0] = 40;
    require(!view.originalIndex(0, actual, read) && actual == UINT32_MAX);
    descriptor.globalStart++;
    require(!view.resolve(address, descriptor, 40, read) && !view.sourceIndices);
    descriptor.globalStart--; descriptor.first = UINT32_MAX;
    require(!view.resolve(address, descriptor, 40, read));
    descriptor.first = 7; descriptor.end++;
    require(!view.resolve(address, descriptor, 40, read));
    std::cout << "PASS reordered40=1 culled1=1 range_rejection=1 setup_reads=3 setup_bytes=16 "
                 "per_instance_bytes=2 table_scans=0 live_engine=0 lifetime_proven=0\n";
}
