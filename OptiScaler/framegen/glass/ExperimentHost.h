#pragma once
#include <d3d12.h>
#include <filesystem>
namespace GlassFg
{
// True means explicit experiment-host mode claimed the capture owner. A startup
// failure remains visible in the log; it must not start a second legacy owner.
bool StartExperimentHost(ID3D12Device*, const std::filesystem::path&) noexcept;
}
