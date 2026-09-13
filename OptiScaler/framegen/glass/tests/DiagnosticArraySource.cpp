#include "../DiagnosticArraySource.h"
#include <cstdio>
#include <cstring>

int main()
{
    std::array<unsigned char, 0x230> node {};
    std::array<unsigned char, 0xe8> definition {};
    std::array<unsigned char, 0x40> buffer {};
    const auto n = reinterpret_cast<std::uint64_t>(node.data());
    const auto d = reinterpret_cast<std::uint64_t>(definition.data());
    const auto b = reinterpret_cast<std::uint64_t>(buffer.data());
    auto put = [](auto& bytes, unsigned offset, std::uint64_t value) { memcpy(bytes.data() + offset, &value, 8); };
    put(node, 0x60, d); put(node, 0x68, 0x11000); put(node, 0xc8, 0x22000);
    put(definition, 0x68, b); put(definition, 0x70, 0x33000); put(definition, 0x78, 100ULL | (8ULL << 32));
    // Deliberately unmapped transforms: only scalar provenance may be read.
    put(buffer, 0x30, 0x40000); put(buffer, 0x38, 32768);
    put(node, 0x1f8, 0x49);
    unsigned reads = 0, transformReads = 0, maskReads = 0;
    bool mutate = false;
    auto read = [&](std::uint64_t address, void* output, unsigned size)
    {
        ++reads;
        const bool valid = (address >= n && address - n + size <= node.size()) ||
                           (address >= d && address - d + size <= definition.size()) ||
                           (address >= b && address - b + size <= buffer.size());
        if (!valid) { ++transformReads; return false; }
        memcpy(output, reinterpret_cast<void*>(address), size);
        if (address == n + 0x1f8 && ++maskReads == 2 && mutate)
            *static_cast<std::uint64_t*>(output) ^= 2;
        return true;
    };
    CONTEXT context {}; context.Rdi = n; context.Rbx = n;
    auto compact = [&] {
        maskReads = 0;
        return GlassFg::ReadDiagnosticArraySource(0x579d30, context, 0x22000, 0x60000, 0x60090, read);
    };
    auto s = compact();
    if (s.kind != 2 || s.flags != 31 || s.first != 100 || s.count != 8 || s.mask[0] != 0x49) return 1;
    put(node, 0x1f8, 0x4a);
    s = compact();
    if (s.flags != 31 || s.mask[0] != 0x4a) return 2;
    mutate = true; s = compact();
    if (s.flags & 8) return 3;
    mutate = false;
    s = GlassFg::ReadDiagnosticArraySource(0x579d30, context, 0x23000, 0x60000, 0x60090, read);
    if (s.flags & 4) return 4;
    s = GlassFg::ReadDiagnosticArraySource(0x57b3e0, context, 0x22000, 0x60000, 0x60090, read);
    if (s.kind != 3 || s.flags != 15) return 5;
    context.Rsi = 8;
    s = GlassFg::ReadDiagnosticArraySource(0x57b3e0, context, 0x22000, 0x60000, 0x60180, read);
    if (s.kind != 3 || s.flags != 31 || s.first != 100 || s.count != 8 || s.mask[0] != 0x4a) return 9;
    context.Rsi = 7;
    s = GlassFg::ReadDiagnosticArraySource(0x57b3e0, context, 0x22000, 0x60000, 0x60180, read);
    if (s.flags & 16) return 10;
    context.Rsi = 8;
    s = GlassFg::ReadDiagnosticArraySource(0x57b3e0, context, 0x22000, 0x60000, 0x60090, read);
    if (s.flags & 16) return 11; // Three active entries do not make a three-entry dynamic span.
    put(node, 0x1f8, 0);
    s = GlassFg::ReadDiagnosticArraySource(0x57b3e0, context, 0x22000, 0x60000, 0x60180, read);
    if (s.flags & 16) return 12;
    put(node, 0x1f8, 0x4a);
    mutate = true; maskReads = 0;
    s = GlassFg::ReadDiagnosticArraySource(0x57b3e0, context, 0x22000, 0x60000, 0x60180, read);
    if (s.flags & 8) return 13;
    mutate = false;
    put(definition, 0x78, 100ULL | (65ULL << 32)); context.Rsi = 65;
    put(node, 0x1f8, 0); put(node, 0x200, 1);
    s = GlassFg::ReadDiagnosticArraySource(0x57b3e0, context, 0x22000, 0x60000, 0x60000 + 65 * 48, read);
    if (s.flags != 31 || s.mask[1] != 1) return 14;
    put(node, 0x200, 2); // Outside this 65-element source domain.
    s = GlassFg::ReadDiagnosticArraySource(0x57b3e0, context, 0x22000, 0x60000, 0x60000 + 65 * 48, read);
    if (s.flags & 16) return 15;
    put(definition, 0x78, 100ULL | (257ULL << 32)); context.Rsi = 257;
    put(node, 0x1f8, 1);
    s = GlassFg::ReadDiagnosticArraySource(0x57b3e0, context, 0x22000, 0x60000, 0x60000 + 257 * 48, read);
    if (s.flags & 16) return 16;
    put(definition, 0x78, 100ULL | (8ULL << 32)); context.Rsi = 8;
    put(node, 0x1f8, 0x4a); put(node, 0x200, 0);
    put(definition, 0x38, b); put(definition, 0x40, 0x33000); put(definition, 0x48, 100ULL | (40ULL << 32));
    s = GlassFg::ReadDiagnosticArraySource(0x3c8657, context, 0x22000,
        0x40000 + 137 * 48, 0x40000 + 140 * 48, read);
    if (s.kind != 1 || s.flags != 27 || s.first != 137 || s.count != 3) return 6;
    s = GlassFg::ReadDiagnosticArraySource(0x3c8657, context, 0x22000,
        0x40001 + 137 * 48, 0x40001 + 140 * 48, read);
    if (s.flags & 16) return 7;
    const auto before = reads;
    s = GlassFg::ReadDiagnosticArraySource(1, context, 0x22000, 0x60000, 0x60090, read);
    if (s.kind || s.flags || reads != before || transformReads) return 8;
    std::puts("PASS array_source=1 compact_mask_changes=1 shared_range=1 changing_snapshot_rejected=1 "
              "dynamic_placeholder_layout=1 wrong_dynamic_counts_rejected=1 "
              "temporal_identity_not_assumed=1 transform_reads=0 game_hooks=0");
}
