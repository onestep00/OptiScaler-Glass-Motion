#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>

namespace GlassFg
{
using NativeEvaluate = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                            const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
NVSDK_NGX_Result EvaluateNativeFG(ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                                  NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback,
                                  NativeEvaluate original);
void RetireNativeFG(const NVSDK_NGX_Handle* handle);
void CreatedNativeFG();
void StopNativeFG();
} // namespace GlassFg
