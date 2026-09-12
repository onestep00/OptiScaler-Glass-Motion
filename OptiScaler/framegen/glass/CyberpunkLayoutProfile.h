#pragma once
#include "RelocatableCode.h"

namespace GlassFg::CyberpunkProfile
{
// Audited instruction-layout profiles, normalized only at address operands.
// No executable timestamp, image-size identity, preferred RVA or driver signature.
using Target = RelocatableCode::Target;
using Reference = RelocatableCode::Reference;
inline constexpr Reference registerReferences[] {
    { 0x46, 0x4a, Target::Writable },
    { 0x5e, 0x62, Target::Code },
    { 0x77, 0x7b, Target::Writable },
    { 0x7c, 0x80, Target::Code },
};
inline constexpr Reference removeReferences[] {
    { 0x17, 0x1b, Target::Writable },
    { 0x2a, 0x2e, Target::Code },
};
inline constexpr Reference updateReferences[] {
    { 0x1f, 0x23, Target::ReadOnly }, { 0x87, 0x8b, Target::Code },       { 0x8f, 0x93, Target::ReadOnly },
    { 0xcf, 0xd3, Target::Code },     { 0x1e4, 0x1e8, Target::Writable }, { 0x1fb, 0x1ff, Target::Code },
};
inline constexpr Reference runReferences[] {
    { 0x36, 0x3a, Target::Code },   { 0x4c, 0x50, Target::Code },       { 0x53, 0x57, Target::Writable },
    { 0x7e, 0x82, Target::Code },   { 0x8c, 0x90, Target::Writable },   { 0x91, 0x95, Target::Code },
    { 0x9e, 0xa2, Target::Code },   { 0xb5, 0xb9, Target::Code },       { 0xd9, 0xdd, Target::Code },
    { 0xfb, 0xff, Target::Code },   { 0x12b, 0x12f, Target::Writable }, { 0x15c, 0x160, Target::Code },
    { 0x167, 0x16b, Target::Code }, { 0x1a5, 0x1a9, Target::Code },     { 0x221, 0x225, Target::Code },
    { 0x291, 0x295, Target::Code }, { 0x2bd, 0x2c1, Target::ReadOnly }, { 0x2c2, 0x2c6, Target::Code },
    { 0x30c, 0x310, Target::Code }, { 0x461, 0x465, Target::Code },     { 0x4ab, 0x4af, Target::Code },
    { 0x4c2, 0x4c6, Target::Code }, { 0x538, 0x53c, Target::Code },     { 0x574, 0x578, Target::Code },
    { 0x5ef, 0x5f3, Target::Code }, { 0x614, 0x618, Target::Code },     { 0x63f, 0x643, Target::Code },
    { 0x663, 0x667, Target::Code }, { 0x679, 0x67d, Target::Code },     { 0x69a, 0x69e, Target::Code },
    { 0x6b3, 0x6b7, Target::Code }, { 0x6e7, 0x6eb, Target::Code },     { 0x6fc, 0x700, Target::Code },
};
inline constexpr Reference appendReferences[] {
    { 0x263, 0x267, Target::ReadOnly }, { 0x289, 0x28d, Target::ReadOnly }, { 0x2f8, 0x2fc, Target::Code },
    { 0x31a, 0x31e, Target::Code },     { 0x349, 0x34d, Target::ReadOnly }, { 0x356, 0x35a, Target::Code },
    { 0x36f, 0x373, Target::Code },     { 0x37c, 0x380, Target::Code },     { 0x396, 0x39a, Target::Code },
};
inline constexpr Reference rigidReferences[] {
    { 0x67, 0x6b, Target::Code },
    { 0x8d, 0x91, Target::Code },
    { 0x9f, 0xa3, Target::Code },
    { 0xcb, 0xcf, Target::Code },
};
inline constexpr Reference skinnedReferences[] {
    { 0x5b, 0x5f, Target::Code },
    { 0x80, 0x84, Target::Code },
    { 0xb9, 0xbd, Target::Code },
};
inline constexpr Reference uploadReferences[] {
    { 0xa7, 0xab, Target::Code },
    { 0xc9, 0xcd, Target::Code },
};
inline constexpr Reference backendReferences[] {
    { 0x40, 0x44, Target::Code },   { 0x75, 0x79, Target::Writable }, { 0xa0, 0xa4, Target::Writable },
    { 0xb9, 0xbd, Target::Code },   { 0xd8, 0xdc, Target::Code },     { 0xe7, 0xeb, Target::Code },
    { 0x1d3, 0x1d7, Target::Code },
};
inline constexpr Reference arrayReferences[] {
    { 0x29, 0x2d, Target::ReadOnly },
    { 0x4d, 0x51, Target::ReadOnly },
    { 0x73, 0x77, Target::Code },
    { 0xfd, 0x101, Target::Writable },
    { 0x134, 0x138, Target::Code },
    { 0x165, 0x169, Target::Code },
    { 0x179, 0x17d, Target::Writable },
    { 0x19a, 0x19e, Target::Code },
    { 0x1b8, 0x1bc, Target::Code },
    { 0x1d5, 0x1d9, Target::Code },
    { 0x1dd, 0x1e1, Target::Code },
    { 0x21e, 0x222, Target::Code },
    { 0x23b, 0x23f, Target::Code },
    { 0x264, 0x268, Target::Code },
    { 0x272, 0x276, Target::Code },
};
inline constexpr RelocatableCode::Profile functions[] {
    { 161, 0x8a953f8e5b8de2b7ull, registerReferences }, { 59, 0x6b1d0915dfa05790ull, removeReferences },
    { 521, 0x1114e8e80bbfb0e6ull, updateReferences },   { 1812, 0x1b01b46553f7def1ull, runReferences },
    { 953, 0x3a170baf4c2407e7ull, appendReferences },   { 237, 0xb2675ba60ce512baull, rigidReferences },
    { 214, 0x8c6d363d91a36eaull, skinnedReferences },   { 219, 0x73cfc17215913d02ull, uploadReferences },
    { 585, 0x62183a4a9eac8718ull, backendReferences },
    { 630, 0x5bccf1fceae351f9ull, arrayReferences },
};
} // namespace GlassFg::CyberpunkProfile
