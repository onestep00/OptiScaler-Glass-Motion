#pragma once
#include <d3d12.h>
namespace sl::param
{
struct IParameters;
}
namespace GlassFg
{
// Called by the existing OptiScaler common-plugin loader after version parsing.
void OnStreamlineCommonLoad(sl::param::IParameters* params, unsigned major, unsigned minor, unsigned patch);
void* WrapStreamlineCommonFunction(const char* name, void* original);
// Native FG host only, once per Evaluate before any input substitution.
bool ReadStreamlineStates(const void* feature, unsigned index, unsigned count, ID3D12Resource* const (&resources)[3],
                          D3D12_RESOURCE_STATES (&states)[3]);
} // namespace GlassFg
