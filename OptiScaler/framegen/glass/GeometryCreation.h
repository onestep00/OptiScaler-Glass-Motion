#pragma once
#include "GeometryPipelineCache.h"

namespace GlassFg
{
struct GeometryCreationStats
{
    bool active = false;
    std::uint64_t roots = 0, graphics = 0, streams = 0;
    GeometryCacheStats cache;
};

// Install once on the actual device implementation, before it is returned to
// the application. Only successful public creations are copied. Hooks forward
// unchanged and do not record GPU commands. Compiler-generated calls are ignored.
// The callback code remains process-resident after installation. Stop releases
// the bounded CPU/PSO cache; returned pipeline leases keep their own resources.
bool StartGeometryCreation(ID3D12Device* device, const std::filesystem::path& compiler,
                           GeometryCacheLimits limits = {}) noexcept;
// Stop joins the compiler. Only call on an owning control thread, never in
// DllMain, a draw callback, or while holding application/driver locks.
void StopGeometryCreation();
GeometryCreationStats GetGeometryCreationStats();
std::shared_ptr<const GeometryPipelineEntry> FindGeometryPipeline(ID3D12PipelineState* original) noexcept;
void ObserveGeometryRoot(ID3D12Device* device, UINT node, const void* bytes, SIZE_T size, IUnknown* created) noexcept;

// Use at the final upstream creation call, after any sampler override and
// reserialization. Observing the outer, pre-override blob would copy a different
// root from the one actually bound by the application.
template <typename Create>
HRESULT CreateObservedGeometryRoot(Create original, ID3D12Device* device, UINT node, const void* bytes, SIZE_T size,
                                   REFIID iid, void** result)
{
    const auto status = original(device, node, bytes, size, iid, result);
    if (SUCCEEDED(status) && result && *result)
        ObserveGeometryRoot(device, node, bytes, size, static_cast<IUnknown*>(*result));
    return status;
}

// Thin OptiScaler adapter: executable admission, module-relative compiler path,
// and one startup diagnostic. It does not enable draw/input replacement.
void InitializeGeometryHost(ID3D12Device* device) noexcept;
} // namespace GlassFg
