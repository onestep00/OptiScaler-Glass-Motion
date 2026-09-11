// Owned CPU memory only. No game, DLL installation or engine hook execution.
#include "../ExperimentInstanceProducer.cpp"
#include <iostream>
#include <stdexcept>

namespace
{
std::array<unsigned char, 0x118> proxyData {};
std::array<unsigned char, 0x58> groupData {};
std::array<std::uint16_t, 40> sourceData {};
unsigned testTick = 50, forwards = 0;
Selection::Descriptor testDescriptor { 20160, 40, 0, 0, 0x10000, 0x10000 + 40 * 48 };
void require(bool okay) { if (!okay) throw std::runtime_error("Producer callback failure"); }
unsigned char fakeOuter(void* a, void*, void*, std::uint64_t depth)
{
    ++forwards;
    if (!depth) { require(!current); return 7; }
    auto* owner = current;
    require(owner && owner->proxy == reinterpret_cast<std::uint64_t>(a));
    observeGroup(groupData.data() + 0x10, groupReturn + 1);
    require(!owner->group);
    observeGroup(groupData.data() + 0x10, groupReturn);
    require(owner->group == reinterpret_cast<std::uint64_t>(groupData.data()));
    // A nested disabled call cannot inherit the outer group.
    enabled = false;
    require(outer(a, nullptr, nullptr, 0) == 7 && current == owner);
    enabled = true;
    observe(a, &testDescriptor);
    require(!owner->group);
    observe(a, &testDescriptor); // Consumed group cannot produce another row.
    return 19;
}
}
int main()
{
    tick = &testTick; groupReturn = 0x123456;
    auto writeValue = [](auto& data, unsigned offset, const auto& value)
    { memcpy(data.data() + offset, &value, sizeof(value)); };
    const std::uint64_t mesh = 0x30000, source = reinterpret_cast<std::uint64_t>(sourceData.data());
    const unsigned slot = 27, count = 40, start = 20160;
    writeValue(proxyData, 0x98, slot); writeValue(proxyData, 0xd8, mesh);
    writeValue(proxyData, 0x110, count);
    writeValue(groupData, 0x18, source); writeValue(groupData, 0x28, count);
    writeValue(groupData, 0x50, start);
    for (unsigned i = 0; i < count; ++i) sourceData[i] = std::uint16_t(39 - i);
    originalOuter = fakeOuter; enabled = true;
    require(outer(proxyData.data(), nullptr, nullptr, 1) == 19);
    require(used == 1 && active == 0 && current == nullptr && forwards == 2);
    require(rows[0].valid == 1 && rows[0].indices[0] == 39 && rows[0].indices[39] == 0);
    Scope scope { reinterpret_cast<std::uint64_t>(proxyData.data()),
                  reinterpret_cast<std::uint64_t>(groupData.data()), testTick };
    current = &scope; ++testTick;
    observe(proxyData.data(), &testDescriptor); require(used == 1);
    current = nullptr; enabled = false;
    std::cout << "PASS current_group_direct=1 wrong_caller_rejected=1 nested_scope=1 consumed_once=1 "
                 "frame_mismatch_rejected=1 original_return_preserved=1 game_hooks_installed=0\n";
}
