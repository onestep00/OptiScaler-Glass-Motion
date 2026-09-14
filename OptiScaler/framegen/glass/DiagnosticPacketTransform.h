#pragma once
#include "ExperimentSourceAbi.h"
#include <array>
namespace GlassFg
{
// Audited1f1208 CPU source only. Not a GPU read or previous-render guarantee.
template<class Read>
bool capturePacketTransform(const GlassExperimentPacketParent& parent, uint32_t ordinal,
                            std::array<uint32_t,12>& result, Read read)
{
    result={};
    constexpr uint64_t limit=0x7fffffffffffULL;
    if(parent.size!=sizeof(parent)||parent.version!=1||parent.reserved||!parent.proxy||!parent.mesh||
       parent.slot>=131072||!parent.count||ordinal>=parent.count||
       uint64_t(parent.transformIndex)+parent.count>131072)return false;
    const uint64_t entryOffset=0x274248+uint64_t(parent.slot)*24;
    if(parent.entry<entryOffset+0x10000)return false;
    const auto renderer=parent.entry-entryOffset;
    const uint64_t offset=0x574280+48*(uint64_t(parent.transformIndex)+ordinal);
    if(renderer>limit-offset-48)return false;
    std::array<uint32_t,12> first{},second{};
    if(!read(renderer+offset,first)||!read(renderer+offset,second)||first!=second)return false;
    result=first;return true;
}
}
