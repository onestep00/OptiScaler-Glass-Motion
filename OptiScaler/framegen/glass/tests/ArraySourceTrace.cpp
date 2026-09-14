#define GLASS_ARRAY_SOURCE_TRACE
#include "../ExperimentInstanceUpdates.cpp"
#include <cstdio>

extern "C" void* DiagnosticWrapperTarget = reinterpret_cast<void*>(&enqueue);
extern "C" unsigned char ArraySourceTraceFixture(void*, void*, void*, void*);
extern "C" void DiagnosticArrayReturn();
extern "C" unsigned char DynamicArraySourceTraceFixture(void*, void*, void*, void*, std::uint64_t);
extern "C" void DiagnosticDynamicArrayReturn();
extern "C" unsigned char SplitArraySourceTraceFixture(void*, void*, void*, void*, void*, void*);
extern "C" void DiagnosticSplitArrayReturn();
unsigned forwards = 0;
unsigned char forward(void*, void*, void*) { ++forwards; return 39; }
int main()
{
    std::array<unsigned char, 0x230> node {};
    std::array<unsigned char, 0xe8> definition {};
    std::array<unsigned char, 0x40> buffer {};
    std::array<std::uint64_t, 3> handle {1, 2, 0x10000};
    std::array<std::uint64_t, 2> span {0x60000, 0x60090};
    std::array<std::uint64_t, 4> bounds {};
    auto put = [](auto& bytes, unsigned offset, std::uint64_t value) { memcpy(bytes.data() + offset, &value, 8); };
    put(node, 0x60, reinterpret_cast<std::uint64_t>(definition.data())); put(node, 0x68, 0x11000);
    put(node, 0xc8, reinterpret_cast<std::uint64_t>(handle.data())); put(node, 0x1f8, 0x49);
    put(definition, 0x68, reinterpret_cast<std::uint64_t>(buffer.data())); put(definition, 0x70, 0x33000);
    put(definition, 0x78, 100ULL | (8ULL << 32));
    put(buffer, 0x30, 0x40000); put(buffer, 0x38, 32768);
    original = forward; recording = true; arraysOnly = true;
    sourceImage = reinterpret_cast<std::uint64_t>(&DiagnosticArrayReturn) - 0x579d30;
    for (unsigned i = 0; i < 2; ++i)
    {
        put(node, 0x1f8, 0x49 + i);
        if (ArraySourceTraceFixture(node.data(), handle.data(), span.data(), bounds.data()) != 39) return 1;
        if (used != i + 1 || rows[i].source.flags != 31 || rows[i].source.kind != 2 ||
            rows[i].source.mask[0] != 0x49ULL + i || rows[i].source.steps != 2 ||
            rows[i].source.node != reinterpret_cast<std::uint64_t>(node.data())) return 2;
    }
    sourceImage = reinterpret_cast<std::uint64_t>(&DiagnosticDynamicArrayReturn) - 0x57b3e0;
    span[1] = span[0] + 8 * 48;
    for (unsigned count : {8u, 7u})
    {
        const auto index = used.load();
        if (DynamicArraySourceTraceFixture(node.data(), handle.data(), span.data(), bounds.data(), count) != 39)
            return 4;
        const auto& source = rows[index].source;
        if (used != index + 1 || source.kind != 3 || source.steps != 2 ||
            source.node != reinterpret_cast<std::uint64_t>(node.data()) ||
            source.flags != (count == 8 ? 31u : 15u) || source.mask[0] != 0x4a) return 5;
    }
    std::array<std::uint32_t, 2> range {100, 8};
    const auto rangeAddress = reinterpret_cast<std::uint64_t>(range.data());
    std::array<std::uint64_t, 8> split {0x100000, 0x100000 + 300 * 12,
        0x200000, 0x200000 + 300 * 8, 0x300000, 0x300000 + 300 * 4,
        rangeAddress, rangeAddress + 8};
    sourceImage = reinterpret_cast<std::uint64_t>(&DiagnosticSplitArrayReturn) - 0x3a2559;
    const auto index = used.load();
    if (SplitArraySourceTraceFixture(node.data(), handle.data(), span.data(), bounds.data(),
        split.data(), range.data()) != 39) return 6;
    const auto& splitSource = rows[index].source;
    if (used != index + 1 || splitSource.kind != 5 || splitSource.flags != 27 ||
        splitSource.steps != 2 || splitSource.first != 100 || splitSource.count != 8 ||
        splitSource.splitSpans != split || splitSource.splitRange != rangeAddress ||
        splitSource.node != reinterpret_cast<std::uint64_t>(node.data()) ||
        splitSource.splitContext != reinterpret_cast<std::uint64_t>(split.data())) return 7;
    const auto recorded = used.load();
    const auto forwarded = forwards;
    recording = false;
    if (ArraySourceTraceFixture(node.data(), handle.data(), span.data(), bounds.data()) != 39 ||
        used != recorded || forwards != forwarded + 1 || active || profileMagic != 0x49555036) return 3;
    std::puts("PASS wrapper_real_caller_context=1 source_mask_changes=1 forwarded_result=39 "
              "dynamic_rbx_rsi_context=1 wrong_dynamic_count_rejected=1 "
              "split_r14_r15_rbx_context=1 "
              "unwind_steps=2 disabled_recording=1 game_hooks=0");
}
