#pragma once

#pragma warning(push, 0)
#include <d3dx/d3dx12.h>
#pragma warning(pop)

namespace GlassFg
{
enum class GeometryPipelineStreamKind : std::uint8_t
{
    Graphics,
    NonGraphics,
    Unsupported,
    Invalid
};

struct GeometryPipelineStreamResult
{
    GeometryPipelineStreamKind kind = GeometryPipelineStreamKind::Invalid;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics {};
};

// Uses Microsoft's public stream parser. Unknown/newer subobjects are rejected
// instead of guessing their ABI, and non-default view instancing is rejected
// because D3D12_GRAPHICS_PIPELINE_STATE_DESC cannot represent it.
inline GeometryPipelineStreamResult ParseGeometryPipelineStream(
    const D3D12_PIPELINE_STATE_STREAM_DESC& source) noexcept
{
    struct Parser final : CD3DX12_PIPELINE_STATE_STREAM_PARSE_HELPER
    {
        bool vertex = false, compute = false, nonDefaultViewInstancing = false, error = false;

        void VSCb(const D3D12_SHADER_BYTECODE& shader) override
        {
            vertex = shader.pShaderBytecode && shader.BytecodeLength;
            CD3DX12_PIPELINE_STATE_STREAM_PARSE_HELPER::VSCb(shader);
        }
        void CSCb(const D3D12_SHADER_BYTECODE& shader) override
        {
            compute = shader.pShaderBytecode && shader.BytecodeLength;
            CD3DX12_PIPELINE_STATE_STREAM_PARSE_HELPER::CSCb(shader);
        }
        void ViewInstancingCb(const D3D12_VIEW_INSTANCING_DESC& view) override
        {
            nonDefaultViewInstancing = view.ViewInstanceCount || view.pViewInstanceLocations ||
                                       view.Flags != D3D12_VIEW_INSTANCING_FLAG_NONE;
            CD3DX12_PIPELINE_STATE_STREAM_PARSE_HELPER::ViewInstancingCb(view);
        }
        void ErrorBadInputParameter(UINT) override { error = true; }
        void ErrorDuplicateSubobject(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) override { error = true; }
        void ErrorUnknownSubobject(UINT) override { error = true; }
    } parser;

    GeometryPipelineStreamResult result;
    if (!source.pPipelineStateSubobjectStream || !source.SizeInBytes || source.SizeInBytes > 1024 * 1024 ||
        FAILED(D3DX12ParsePipelineStream(source, &parser)) || parser.error)
        return result;
    if (parser.compute)
    {
        result.kind = parser.vertex ? GeometryPipelineStreamKind::Invalid
                                    : GeometryPipelineStreamKind::NonGraphics;
        return result;
    }
    if (!parser.vertex)
    {
        result.kind = GeometryPipelineStreamKind::Unsupported; // Mesh or a future graphics pipeline.
        return result;
    }
    if (parser.nonDefaultViewInstancing)
    {
        result.kind = GeometryPipelineStreamKind::Unsupported;
        return result;
    }
    result.graphics = parser.PipelineStream.GraphicsDescV0();
    result.kind = GeometryPipelineStreamKind::Graphics;
    return result;
}
} // namespace GlassFg
