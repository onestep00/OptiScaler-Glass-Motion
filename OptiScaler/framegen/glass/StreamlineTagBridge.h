#pragma once
#include <d3d12.h>
#include <cstdint>
namespace sl::param
{
struct IParameters;
}
namespace GlassFg
{
struct StreamlineInputFrame
{
    std::uint64_t frame = UINT64_MAX;
    std::uint32_t viewport = 0;
};
// Called by the existing OptiScaler common-plugin loader after version parsing.
void OnStreamlineCommonLoad(sl::param::IParameters* params, unsigned major, unsigned minor, unsigned patch);
void* WrapStreamlineCommonFunction(const char* name, void* original);
// Native FG host only, once per Evaluate before any input substitution.
bool ReadStreamlineStates(const void* feature, unsigned index, unsigned count, ID3D12Resource* const (&resources)[3],
                          D3D12_RESOURCE_STATES (&states)[3], StreamlineInputFrame* frame = nullptr);
} // namespace GlassFg
