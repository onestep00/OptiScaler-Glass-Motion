#pragma once
#include "RelocatableCode.h"

namespace GlassFg::CyberpunkDeclarations {
// Address operands are normalized; field offsets, types and branches remain
// checked. These are audited code layouts, not a driver or executable version test.
using Reference = RelocatableCode::Reference;
using Target = RelocatableCode::Target;
inline constexpr Reference metadataReferences[] {
    { 62, 66, Target::Code },
};
inline constexpr RelocatableCode::Profile metadataProfile { 113, 0x9de342a0cf10c4bdULL, metadataReferences };
inline constexpr Reference lookupReferences[] {
    { 20, 24, Target::Code },
};
inline constexpr RelocatableCode::Profile lookupProfile { 50, 0xc96be01d797c2ebaULL, lookupReferences };
inline constexpr Reference builderReferences[] {
    { 107, 111, Target::Code },
    { 118, 122, Target::Code },
    { 127, 131, Target::Code },
    { 135, 139, Target::Code },
    { 150, 154, Target::Writable },
    { 162, 166, Target::Code },
    { 191, 195, Target::Code },
    { 220, 224, Target::Code },
    { 240, 244, Target::Code },
    { 290, 294, Target::Code },
    { 311, 315, Target::Code },
    { 426, 430, Target::Code },
    { 435, 439, Target::Code },
    { 456, 460, Target::Code },
    { 475, 479, Target::Code },
    { 484, 488, Target::Code },
    { 505, 509, Target::Code },
    { 524, 528, Target::Code },
    { 533, 537, Target::Code },
    { 554, 558, Target::Code },
    { 573, 577, Target::Code },
    { 582, 586, Target::Code },
    { 603, 607, Target::Code },
    { 623, 627, Target::Code },
    { 633, 637, Target::Code },
    { 655, 659, Target::Code },
    { 674, 678, Target::Code },
    { 683, 687, Target::Code },
    { 704, 708, Target::Code },
    { 724, 728, Target::Code },
    { 734, 738, Target::Code },
    { 756, 760, Target::Code },
    { 776, 780, Target::Code },
    { 786, 790, Target::Code },
    { 808, 812, Target::Code },
    { 827, 831, Target::Code },
    { 836, 840, Target::Code },
    { 857, 861, Target::Code },
    { 931, 935, Target::Code },
    { 940, 944, Target::Code },
    { 961, 965, Target::Code },
    { 981, 985, Target::Code },
    { 991, 995, Target::Code },
    { 1013, 1017, Target::Code },
    { 1033, 1037, Target::Code },
    { 1043, 1047, Target::Code },
    { 1065, 1069, Target::Code },
    { 1085, 1089, Target::Code },
    { 1095, 1099, Target::Code },
    { 1117, 1121, Target::Code },
    { 1136, 1140, Target::Code },
    { 1145, 1149, Target::Code },
    { 1166, 1170, Target::Code },
    { 1186, 1190, Target::Code },
    { 1196, 1200, Target::Code },
    { 1218, 1222, Target::Code },
    { 1237, 1241, Target::Code },
    { 1246, 1250, Target::Code },
    { 1267, 1271, Target::Code },
    { 1286, 1290, Target::Code },
    { 1295, 1299, Target::Code },
    { 1316, 1320, Target::Code },
    { 1405, 1409, Target::Code },
    { 1414, 1418, Target::Code },
    { 1435, 1439, Target::Code },
    { 1455, 1459, Target::Code },
    { 1465, 1469, Target::Code },
    { 1487, 1491, Target::Code },
    { 1506, 1510, Target::Code },
    { 1515, 1519, Target::Code },
    { 1536, 1540, Target::Code },
    { 1555, 1559, Target::Code },
    { 1564, 1568, Target::Code },
    { 1585, 1589, Target::Code },
    { 1605, 1609, Target::Code },
    { 1630, 1634, Target::Code },
    { 1650, 1654, Target::Code },
    { 1660, 1664, Target::Code },
    { 1682, 1686, Target::Code },
    { 1701, 1705, Target::Code },
    { 1710, 1714, Target::Code },
    { 1731, 1735, Target::Code },
    { 1751, 1755, Target::Code },
    { 1761, 1765, Target::Code },
    { 1783, 1787, Target::Code },
    { 1857, 1861, Target::Code },
    { 1866, 1870, Target::Code },
    { 1887, 1891, Target::Code },
    { 1906, 1910, Target::Code },
    { 1915, 1919, Target::Code },
    { 1936, 1940, Target::Code },
    { 1955, 1959, Target::Code },
    { 1964, 1968, Target::Code },
    { 1985, 1989, Target::Code },
    { 2005, 2009, Target::Code },
    { 2015, 2019, Target::Code },
    { 2037, 2041, Target::Code },
    { 2054, 2058, Target::Code },
    { 2064, 2068, Target::Code },
    { 2082, 2086, Target::Code },
    { 2099, 2103, Target::Code },
    { 2113, 2117, Target::Code },
    { 2130, 2134, Target::Code },
    { 2140, 2144, Target::Code },
    { 2158, 2162, Target::Code },
    { 2168, 2172, Target::Code },
    { 2197, 2201, Target::Writable },
    { 2255, 2259, Target::Code },
    { 2267, 2271, Target::Code },
    { 2302, 2306, Target::Code },
    { 2376, 2380, Target::Code },
    { 2398, 2402, Target::Code },
    { 2414, 2418, Target::Code },
    { 2429, 2433, Target::Code },
    { 2456, 2460, Target::Code },
};
inline constexpr RelocatableCode::Profile builderProfile { 2523, 0x70fc02d9cd7cbe44ULL, builderReferences };
inline constexpr Reference supplierReferences[] {
    { 118, 122, Target::Code },
    { 149, 153, Target::ReadOnly },
    { 156, 160, Target::ReadOnly },
    { 187, 191, Target::Code },
    { 209, 213, Target::Code },
    { 218, 222, Target::ReadOnly },
    { 257, 261, Target::Code },
    { 270, 274, Target::Code },
    { 277, 281, Target::ReadOnly },
    { 315, 319, Target::Code },
    { 338, 342, Target::Code },
    { 360, 364, Target::Code },
    { 388, 392, Target::Code },
};
inline constexpr RelocatableCode::Profile supplierProfile { 628, 0x818ea1357648a21bULL, supplierReferences };
inline constexpr Reference uploaderReferences[] {
    { 31, 35, Target::Code },
    { 50, 54, Target::Code },
    { 60, 64, Target::Code },
};
inline constexpr RelocatableCode::Profile uploaderProfile { 75, 0xe484ab87d78f103dULL, uploaderReferences };
inline constexpr Reference stageReferences[] {
    { 42, 46, Target::Writable },
    { 116, 120, Target::Code },
    { 192, 196, Target::Writable },
    { 258, 262, Target::Code },
};
inline constexpr RelocatableCode::Profile stageProfile { 300, 0x81c2b666af67a8fbULL, stageReferences };
// Return after the checked provider +0x58 call in stageProfile.
inline constexpr uint32_t StageMetadataReturn = 216;
struct Layout {
    uint32_t metadata = 0, lookup = 0, builder = 0, supplier = 0, uploader = 0, stage = 0;
    bool resolve(const RelocatableCode& image) {
        metadata = image.unique(metadataProfile);
        lookup = metadata ? image.referenced(metadata, metadataReferences[0]) : 0;
        builder = image.unique(builderProfile);
        supplier = image.unique(supplierProfile);
        uploader = image.unique(uploaderProfile);
        stage = image.unique(stageProfile);
        return metadata && lookup && builder && supplier && uploader && stage &&
               image.functionAt(lookup, lookupProfile);
    }
};
}
