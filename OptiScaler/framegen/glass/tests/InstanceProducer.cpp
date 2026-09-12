// Owned CPU memory only. No game, DLL installation or engine hook execution.
#define GLASS_INSTANCE_LOOKUP
#include "../ExperimentInstanceProducer.cpp"
#include <iostream>
#include <stdexcept>

namespace
{
std::array<unsigned char, 0x118> proxyData {};
std::array<unsigned char, 0x58> groupData {};
std::array<std::uint16_t, 40> sourceData {};
unsigned testTick = 50, forwards = 0;
unsigned sourceQueries = 0, sourceMode = 0;
std::int32_t querySource(std::uint64_t proxy, std::uint64_t mesh, std::uint32_t count,
                         GlassExperimentSourceOwner* result)
{
    if (proxy != reinterpret_cast<std::uint64_t>(proxyData.data()) || mesh != 0x30000 || count != 40)
        throw std::runtime_error("Wrong source query input");
    ++sourceQueries;
    if (sourceMode == 2) throw std::runtime_error("Diagnostic query failure");
    *result = {};
    result->node = 0x40000; result->buffer = 0x50000; result->generation = 17;
    result->first = 107; result->count = sourceMode == 1 ? 39 : 40;
    return 1;
}
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
    require(!rows[0].producerHeaderValid);
    std::array<std::uint64_t, 3> header { 0x20000, 0x30000, 0x40000 };
    require(outer(proxyData.data(), groupData.data(), header.data(), 1) == 19);
    require(used == 2 && rows[1].producerHeaderValid && rows[1].producerHeader == header);
    require(rows[1].outerContext == reinterpret_cast<std::uint64_t>(groupData.data()));
    require(rows[1].producerContext == reinterpret_cast<std::uint64_t>(header.data()));
    GlassExperimentInstanceSource lookup;
    require(GlassInstanceSourceQuery(testTick, mesh, start, count, &lookup) == 1 &&
            lookup.proxy == reinterpret_cast<std::uint64_t>(proxyData.data()) && lookup.indices[0] == 39 && lookup.indices[39] == 0 &&
            lookup.renderer == header[0] && lookup.scene == header[2] && lookup.originalCount == 40);
    require(GlassInstanceSourceQuery(testTick + 1, mesh, start, count, &lookup) == 0);
    require(GlassInstanceSourceQuery(testTick, mesh, start, count - 1, &lookup) == 0);
    GlassFg::DiagnosticInstanceSource duplicate;
    require(instanceLookup.query(testTick, mesh, start, count, duplicate));
    ++duplicate.scene;
    require(!instanceLookup.publish(duplicate));
    require(GlassInstanceSourceQuery(testTick, mesh, start, count, &lookup) == 0);
    Scope scope { reinterpret_cast<std::uint64_t>(proxyData.data()),
                  reinterpret_cast<std::uint64_t>(groupData.data()), testTick };
    current = &scope; ++testTick;
    observe(proxyData.data(), &testDescriptor); require(used == 2);
    scope.frame = testTick; scope.group = 0;
    writeValue(proxyData, 0x114, start);
    Selection::Descriptor linearDescriptor {start, count, 0, 0, 0, 0};
    observe(proxyData.data(), &linearDescriptor, true);
    require(used == 3 && rows[2].linear && !rows[2].group && !rows[2].sourceIndices);
    require(rows[2].indices[0] == 0 && rows[2].indices[39] == 39);
    // An absent group must never silently select the whole-array convention.
    observe(proxyData.data(), &linearDescriptor); require(used == 3);
    ++linearDescriptor.first;
    observe(proxyData.data(), &linearDescriptor, true); require(used == 3);
    linearDescriptor.first = 0; --linearDescriptor.count;
    observe(proxyData.data(), &linearDescriptor, true); require(used == 3);
    linearDescriptor.count = count; ++linearDescriptor.globalStart;
    observe(proxyData.data(), &linearDescriptor, true); require(used == 3);
    linearDescriptor.globalStart = start; linearDescriptor.begin = 0x10000;
    observe(proxyData.data(), &linearDescriptor, true); require(used == 3);
    linearDescriptor.begin = 0; sourceQuery = querySource;
    observe(proxyData.data(), &linearDescriptor, true);
    observe(proxyData.data(), &linearDescriptor, true);
    require(used == 5 && sourceQueries == 1 && rows[3].sourceOwner.generation == 17 &&
            rows[4].sourceOwner.first + rows[4].indices[39] == 146);
    scope.sourceChecked = false; scope.sourceOwner = {}; sourceMode = 1;
    observe(proxyData.data(), &linearDescriptor, true);
    require(used == 6 && sourceQueries == 2 && !rows[5].sourceOwner.generation);
    scope.sourceChecked = false; scope.sourceOwner = {}; sourceMode = 2;
    observe(proxyData.data(), &linearDescriptor, true);
    require(used == 7 && sourceQueries == 3 && !rows[6].sourceOwner.generation);
    current = nullptr; enabled = false;
    std::cout << "PASS current_group_direct=1 wrong_caller_rejected=1 nested_scope=1 consumed_once=1 "
                 "frame_mismatch_rejected=1 original_return_preserved=1 linear_source=1 "
                 "no_missing_group_fallback=1 malformed_linear_rejected=1 source_query_once=1 "
                 "source_mismatch_rejected=1 source_exception_contained=1 direct_lookup=1 "
                 "lookup_frame_count_bounds=1 ambiguous_scene_rejected=1 game_hooks_installed=0\n";
}
