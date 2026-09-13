#pragma once
#include "DiagnosticCallerContext.h"
#include "GeometrySourceSlots.h"
#include "GeometrySourceIndexMap.h"
#include <array>

namespace GlassFg
{
// Scalar observations in the active upstream invocation. No temporal admission.
struct DiagnosticArraySource
{
    std::uint64_t node = 0, definition = 0, definitionControl = 0;
    std::uint64_t buffer = 0, bufferControl = 0, data = 0;
    std::array<std::uint64_t, 4> mask {};
    std::uint32_t first = 0, count = 0, bytes = 0, kind = 0, steps = 0, flags = 0;
};

template<class Read> DiagnosticArraySource ReadDiagnosticArraySource(
    std::uint64_t callerRva, const CONTEXT& caller, std::uint64_t rendererHandle,
    std::uint64_t begin, std::uint64_t end, Read read)
{
    DiagnosticArraySource result;
    switch (callerRva)
    {
    case 0x3c8657: result.kind = 1; result.node = caller.Rdi; break;
    case 0x2276d19: result.kind = 1;
        if (!read(caller.Rbx, &result.node, 8)) return result;
        break;
    case 0x579d30: result.kind = 2; result.node = caller.Rdi; break;
    case 0x57b3e0: result.kind = 3; result.node = caller.Rbx; break;
    case 0xa0457a: result.kind = 4; result.node = caller.Rdi; break;
    case 0x3a2559: result.kind = 5; result.node = caller.R14; break;
    default: return result;
    }
    if (result.node < 0x10000 || result.node > 0x7fffffffffffULL - 0x230) return result;
    std::array<std::uint64_t, 2> definition {};
    if (!read(result.node + 0x60, definition.data(), 16)) return result;
    result.definition = definition[0]; result.definitionControl = definition[1];
    result.flags |= 1;
    // The split population route needs a separate layout/lifetime audit. Keep
    // its node provenance without interpreting resource pointers as mesh ranges.
    if (result.kind == 5) return result;
    if (result.definition < 0x10000 || result.definition > 0x7fffffffffffULL - 0xe8) return result;
    const auto offset = result.kind == 1 ? 0x38u : 0x68u;
    std::array<std::uint64_t, 3> source {}, again {};
    std::array<std::uint64_t, 2> definitionAgain {};
    if (!read(result.definition + offset, source.data(), 24)) return result;
    result.buffer = source[0]; result.bufferControl = source[1];
    result.first = static_cast<unsigned>(source[2]); result.count = static_cast<unsigned>(source[2] >> 32);
    if (result.buffer < 0x10000 || result.buffer > 0x7fffffffffffULL - 0x3c || !result.count ||
        !read(result.buffer + 0x30, &result.data, 8) || !read(result.buffer + 0x38, &result.bytes, 4)) return result;
    const unsigned stride = result.kind == 1 ? 48u : 32u;
    if (std::uint64_t(result.first) + result.count > result.bytes / stride ||
        result.data < 0x10000 || result.data > 0x7fffffffffffULL - result.bytes) return result;
    result.flags |= 2;
    if (result.kind == 2 || result.kind == 3)
    {
        std::uint64_t handle = 0;
        if (!read(result.node + 0xc8, &handle, 8) || handle != rendererHandle ||
            !read(result.node + 0x1f8, result.mask.data(), 32)) return result;
        result.flags |= 4;
    }
    if (!read(result.definition + offset, again.data(), 24) || again != source ||
        !read(result.node + 0x60, definitionAgain.data(), 16) || definitionAgain != definition) return result;
    std::array<std::uint64_t, 4> maskAgain {};
    if ((result.kind == 2 || result.kind == 3) &&
        (!read(result.node + 0x1f8, maskAgain.data(), 32) || maskAgain != result.mask)) return result;
    std::uint64_t dataAgain = 0;
    unsigned bytesAgain = 0;
    if (!read(result.buffer + 0x30, &dataAgain, 8) || dataAgain != result.data ||
        !read(result.buffer + 0x38, &bytesAgain, 4) || bytesAgain != result.bytes) return result;
    result.flags |= 8;
    if (begin < 0x10000 || begin >= end || end > 0x7fffffffffffULL || (end - begin) % 48 != 0) return result;
    const auto packedCount = (end - begin) / 48;
    if (result.kind == 1)
    {
        const auto range = GeometrySourceSpan::resolve(result.data, result.bytes, result.first,
                                                       result.count, begin, end, stride);
        if (range) { result.first = range.first; result.count = range.count; result.flags |= 16; }
    }
    else if (result.kind == 2 && result.count <= 256)
    {
        GeometrySourceIndexMap<> mapping;
        if (mapping.compact(result.first, result.count, result.mask) &&
            mapping.packedCount() == packedCount) result.flags |= 16;
    }
    else if (result.kind == 3 && result.count <= 256 && caller.Rsi == result.count &&
             packedCount == result.count)
    {
        // Native 57b114 advances its input by 64 bytes and appends one 48-byte
        // output for every source ordinal. Inactive bits append placeholders;
        // they do not compact the following elements. RSI is the original
        // input count retained in the checked producer's nonvolatile register.
        // This certifies ordinal layout only, never N-1 identity or visibility.
        for (unsigned word = 0; word < (result.count + 63u) / 64u; ++word)
        {
            const auto remaining = result.count - word * 64u;
            const auto validBits = remaining >= 64 ? UINT64_MAX : (std::uint64_t {1} << remaining) - 1;
            if (result.mask[word] & validBits) { result.flags |= 16; break; }
        }
    }
    else if (result.kind == 4 && packedCount == result.count) result.flags |= 16;
    return result;
}
} // namespace GlassFg
