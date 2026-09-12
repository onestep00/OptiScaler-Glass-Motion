// Owned CPU callbacks only. No game hooks or GPU device.
#include "../ExperimentPacketParents.cpp"
#include "../CyberpunkInstanceSelection.h"
#include <cstdio>
#include <stdexcept>

namespace
{
std::vector<std::byte> rendererMemory(0x280000), rootMemory(0x4630);
std::array<std::byte, 0x120> proxy;
uint64_t root = reinterpret_cast<uint64_t>(rootMemory.data());
Geometry geometry {};
Context context {};
unsigned forwardedAppends = 0, forwardedFlushes = 0, accepted = 0;
bool mutateScope = false;
void require(bool value) { if (!value) throw std::runtime_error("Packet parent fixture failed"); }
template<class T> void put(void* target, size_t offset, T value)
{ memcpy(static_cast<std::byte*>(target) + offset, &value, sizeof(value)); }
void originalAppendFixture(void*, void* packet, uintptr_t c, uintptr_t d, void* ctx)
{
    require(c == 13 && d == 17 && ctx == &context);
    const auto words = static_cast<const uint64_t*>(packet);
    auto& count = (words[1] & (1ULL << 50)) ? context.skinCount : context.rigidCount;
    count += static_cast<unsigned>((words[1] >> 18) & 0x7fff); ++forwardedAppends;
}
void checkFlush()
{
    if (mutateScope) ++batch.rigid.revision;
    GlassExperimentPacketParent value;
    require(GlassPacketParentQuery(drawReturn + 1, geometry.mesh, 7, 2, 0, 0, 2, 30, 1, &value) == 0);
    if (GlassPacketParentQuery(drawReturn, geometry.mesh, 7, 2, 0, 0, 2, 30, 1, &value) == 1)
    {
        require(value.proxy == reinterpret_cast<uint64_t>(proxy.data()) && value.slot == 4 && value.arrayCount == 40 &&
                value.sourceArray == 0x50000 && value.arrayGlobalStart == 20 && value.first == 0 && value.count == 2);
        ++accepted;
    }
    require(GlassPacketParentQuery(drawReturn, geometry.mesh, 7, 2, 1, 0, 2, 30, 1, &value) == 0);
    require(GlassPacketParentQuery(drawReturn, geometry.mesh, 7, 2, 0, 0, 2, 31, 1, &value) == 0);
    ++forwardedFlushes;
}
void originalRigidFixture(void*, void* geo, void* ctx, uint32_t start, bool half)
{ require(geo == &geometry && ctx == &context && start == 30 && !half); checkFlush(); }
void originalSkinnedFixture(void*, void* geo, void* ctx, bool half)
{ require(geo == &geometry && ctx == &context && !half); checkFlush(); }
void appendFixture(bool skin)
{
    std::array<uint64_t, 2> words {1ULL << 59,
        4 | (2ULL << 18) | (30ULL << 33) | (1ULL << 51) | (skin ? 1ULL << 50 : 0)};
    append(nullptr, words.data(), 13, 17, &context);
}
}
int main(int argc, char** argv)
{
    try
    {
        put(rootMemory.data(), 0x4628, reinterpret_cast<uint64_t>(rendererMemory.data()));
        put(rendererMemory.data(), 0x274248 + 4*24, reinterpret_cast<uint64_t>(proxy.data()) | (0xa5ULL << 56));
        put(proxy.data(), 0x98, uint32_t(4)); put(proxy.data(), 0xd8, uint64_t(0x90000));
        put(proxy.data(), 0x108, uint64_t(0x50000)); put(proxy.data(), 0x110, uint32_t(40));
        put(proxy.data(), 0x114, uint32_t(20));
        rendererGlobal = reinterpret_cast<uint64_t>(&root); drawReturn = 0x123456;
        geometry.mesh = 0x90000; geometry.chunk = 7;
        context.geometry = reinterpret_cast<uint64_t>(&geometry);
        context.entry = reinterpret_cast<uint64_t>(rendererMemory.data()) + 0x274248 + 4*24;
        originalAppend = originalAppendFixture; originalRigid = originalRigidFixture; originalSkinned = originalSkinnedFixture;
        enabled = true;
        appendFixture(false); rigid(nullptr, &geometry, &context, 30, false); context.rigidCount = 0;
        appendFixture(true); skinned(nullptr, &geometry, &context, false); context.skinCount = 0;
        require(accepted == 2 && !current);
        GlassExperimentPacketParent value;
        require(GlassPacketParentQuery(drawReturn, geometry.mesh, 7, 2, 0, 0, 2, 30, 1, &value) == 0);
        // Resetting the recording epoch invalidates already accumulated inputs.
        appendFixture(false); ++epoch;
        rigid(nullptr, &geometry, &context, 30, false); context.rigidCount = 0;
        require(accepted == 2);
        // A mismatched native slot is preserved as an unknown interval.
        put(proxy.data(), 0x98, uint32_t(5));
        appendFixture(false); rigid(nullptr, &geometry, &context, 30, false); context.rigidCount = 0;
        put(proxy.data(), 0x98, uint32_t(4));
        appendFixture(false); mutateScope = true;
        rigid(nullptr, &geometry, &context, 30, false); context.rigidCount = 0;
        require(accepted == 2 && forwardedAppends == 5 && forwardedFlushes == 5 && !active);
        std::puts("PASS same_flush_parent=1 rigid_skinned=1 wrong_callsite_range_rejected=1 "
                  "expired_scope_epoch_rejected=1 mutated_batch_rejected=1 invalid_slot_rejected=1 "
                  "original_calls_preserved=1 game_hooks=0 gpu_copies=0");
        if (argc == 2)
        {
            struct Recorded { uint64_t index; GlassExperimentPacketParent source; };
            static_assert(sizeof(Recorded) == 80);
            const std::filesystem::path path = argv[1];
            require(std::filesystem::file_size(path) % sizeof(Recorded) == 0);
            std::ifstream file(path, std::ios::binary);
            Recorded record;
            uint64_t rows = 0, arrayRows = 0, resolved = 0, elements = 0;
            while (file.read(reinterpret_cast<char*>(&record), sizeof(record)))
            {
                const auto& s = record.source;
                require(s.size == sizeof(s) && s.version == 1 && s.proxy && s.mesh && !s.reserved);
                ++rows;
                if (!s.sourceArray && !s.arrayCount && s.arrayGlobalStart == UINT32_MAX) continue;
                ++arrayRows;
                unsigned first = UINT32_MAX;
                if (!GlassFg::CyberpunkInstanceSelection::globalStorageRange(s.globalRange == 1,
                    s.transformIndex, s.count, s.arrayGlobalStart, s.arrayCount, first)) continue;
                require(uint64_t(first) + s.count <= s.arrayCount);
                // ABI v1 did not record flags. These are storage offsets only;
                // do not admit them as original source indices or MV identity.
                ++resolved; elements += s.count;
            }
            require(file.eof());
            std::printf("RECORDED rows=%llu array_rows=%llu global_storage_ranges=%llu elements=%llu original_order_proven=0 "
                        "native_buffer_reads=0 previous_motion_proven=0\n", rows, arrayRows, resolved, elements);
        }
    }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
