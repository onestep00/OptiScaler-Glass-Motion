#pragma once
#include <d3d12.h>

namespace GlassFg
{
enum class MaterialCapture
{
    Transmittance,
    SourceColor,
    Uncovered
};

// For a verified dual-source RGB blend C = F + T * B only.
// Use a separate single-target PSO and owned target. This does not admit a draw,
// validate shader side effects, restore state, or manage GPU resource lifetime.
inline D3D12_BLEND_DESC materialCaptureBlend(MaterialCapture mode)
{
    D3D12_BLEND_DESC result {};
    auto& rt = result.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = mode == MaterialCapture::SourceColor ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
    rt.DestBlend = mode == MaterialCapture::Uncovered ? D3D12_BLEND_ZERO : D3D12_BLEND_SRC1_COLOR;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return result;
}

// Classify the observed RGB equation rather than a material name. These source
// and destination factors do not depend on the owned destination's contents.
// Success only validates the blend algebra, not the draw or its pipeline.
inline bool tryMaterialCaptureBlend(const D3D12_BLEND_DESC& original, MaterialCapture mode, D3D12_BLEND_DESC& capture)
{
    const auto& rt = original.RenderTarget[0];
    const bool source =
        rt.SrcBlend == D3D12_BLEND_ZERO || rt.SrcBlend == D3D12_BLEND_ONE || rt.SrcBlend == D3D12_BLEND_SRC_ALPHA;
    const bool destination = rt.DestBlend == D3D12_BLEND_ZERO || rt.DestBlend == D3D12_BLEND_ONE ||
                             rt.DestBlend == D3D12_BLEND_SRC_ALPHA || rt.DestBlend == D3D12_BLEND_INV_SRC_ALPHA ||
                             rt.DestBlend == D3D12_BLEND_SRC1_COLOR;
    constexpr UINT rgb = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (!rt.BlendEnable || rt.LogicOpEnable || original.AlphaToCoverageEnable || original.IndependentBlendEnable ||
        rt.BlendOp != D3D12_BLEND_OP_ADD || (rt.RenderTargetWriteMask & rgb) != rgb || !source || !destination)
        return false;
    auto result = materialCaptureBlend(mode);
    if (mode == MaterialCapture::SourceColor)
        result.RenderTarget[0].SrcBlend = rt.SrcBlend;
    if (mode != MaterialCapture::Uncovered)
        result.RenderTarget[0].DestBlend = rt.DestBlend;
    capture = result;
    return true;
}

// Clear SourceColor RGB to zero; clear the other modes to one. Alpha is unused.
// Uncovered records zero wherever the original PS survives discard/depth tests,
// including T == 1 pixels. It is coverage of selected draws, not final opacity.
// Across the same ordered draws, SourceColor accumulates F and Transmittance T.
// Interleaved omitted color writes and background-dependent PS inputs require
// separate validation before interpreting these as final scene layers.
} // namespace GlassFg
