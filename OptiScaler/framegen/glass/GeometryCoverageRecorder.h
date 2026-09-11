#pragma once
#include <d3d12.h>
#include <filesystem>

namespace GlassFg
{
// One-shot diagnostic, not a continuous correction path. The request file
// contains an absolute output directory. Nothing is enabled without that file.
// Resources stay process-resident; storage is bounded and never reused in flight.
void StartGeometryCoverageRecorder(ID3D12Device*, const std::filesystem::path& compiler,
                                   const std::filesystem::path& request) noexcept;
}
