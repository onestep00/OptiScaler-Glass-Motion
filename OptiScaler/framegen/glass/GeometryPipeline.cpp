#include "pch.h"
#include "GeometryPipeline.h"
#include "DxilVertexHistory.h"
#include "MaterialCaptureBlend.h"
#include <dxcapi.h>
#include <array>
#include <stdexcept>

namespace GlassFg
{
using Microsoft::WRL::ComPtr;
namespace
{
HRESULT reject(std::string& error, const char* reason, HRESULT result = E_INVALIDARG)
{
    error = reason;
    return result;
}
bool readOnly(const D3D12_DEPTH_STENCIL_DESC& state)
{
    if (state.DepthEnable && state.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO)
        return false;
    const auto keep = [](const D3D12_DEPTH_STENCILOP_DESC& face)
    {
        return face.StencilFailOp == D3D12_STENCIL_OP_KEEP && face.StencilDepthFailOp == D3D12_STENCIL_OP_KEEP &&
               face.StencilPassOp == D3D12_STENCIL_OP_KEEP;
    };
    return !state.StencilEnable || !state.StencilWriteMask || (keep(state.FrontFace) && keep(state.BackFace));
}
} // namespace

HRESULT CreateGeometryRoot(ID3D12Device* device, ID3D12RootSignature* identity, UINT nodeMask, const void* serialized,
                           SIZE_T bytes, GeometryRoot& output, std::string& error, GeometryLayout layout)
{
    error.clear();
    if (!device || !identity || !serialized || !bytes || bytes > 1024 * 1024 ||
        (layout != GeometryLayout::Contiguous && layout != GeometryLayout::PerInstance))
        return reject(error, "Missing or oversized original root signature");
    ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer(serialized, bytes, IID_PPV_ARGS(&deserializer));
    if (FAILED(hr))
        return reject(error, "Cannot deserialize original root", hr);
    const auto* raw = deserializer->GetUnconvertedRootSignatureDesc();
    if (!raw || (raw->Version != D3D_ROOT_SIGNATURE_VERSION_1_0 && raw->Version != D3D_ROOT_SIGNATURE_VERSION_1_1))
        return reject(error, "Unsupported original root version");
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* converted = nullptr;
    hr = deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &converted);
    if (FAILED(hr) || !converted)
        return reject(error, "Cannot convert original root", FAILED(hr) ? hr : E_FAIL);
    auto desc = converted->Desc_1_1;
    constexpr UINT blocked = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE |
                             D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                             D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
    if ((desc.Flags & blocked) || desc.NumParameters > (layout == GeometryLayout::PerInstance ? 58u : 59u) ||
        desc.NumStaticSamplers > 2048)
        return reject(error, "Unsupported root flags or parameter count");

    GeometryRoot result;
    result.originalSerialized.assign(static_cast<const std::byte*>(serialized),
                                     static_cast<const std::byte*>(serialized) + bytes);
    result.originalNodeMask = nodeMask;
    result.layout = layout;
    result.ranges.resize(desc.NumParameters);
    if (desc.NumParameters)
        result.originalParameters.assign(desc.pParameters, desc.pParameters + desc.NumParameters);
    UINT cost = 0;
    for (UINT i = 0; i < desc.NumParameters; ++i)
    {
        auto& p = result.originalParameters[i];
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        {
            ++cost;
            const auto& table = p.DescriptorTable;
            if (table.NumDescriptorRanges > 4096)
                return reject(error, "Oversized root table");
            if (table.NumDescriptorRanges)
                result.ranges[i].assign(table.pDescriptorRanges, table.pDescriptorRanges + table.NumDescriptorRanges);
            for (const auto& range : result.ranges[i])
                if (range.RegisterSpace == 31)
                    return reject(error, "Original root uses reserved capture register space");
            p.DescriptorTable.pDescriptorRanges = result.ranges[i].data();
        }
        else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
        {
            if (p.Constants.RegisterSpace == 31 || p.Constants.Num32BitValues > 64)
                return reject(error, "Original root constant collision or size");
            cost += p.Constants.Num32BitValues;
        }
        else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV || p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ||
                 p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV)
        {
            if (p.Descriptor.RegisterSpace == 31)
                return reject(error, "Original root descriptor collision");
            cost += 2;
        }
        else
            return reject(error, "Unknown root parameter type");
    }
    for (UINT i = 0; i < desc.NumStaticSamplers; ++i)
        if (desc.pStaticSamplers[i].RegisterSpace == 31)
            return reject(error, "Original static sampler uses reserved space");
    const UINT extraCost = layout == GeometryLayout::PerInstance ? 18 : 16;
    if (cost > 64 - extraCost)
        return reject(error, "Capture would exceed 64 DWORD root limit");
    auto parameters = result.originalParameters;
    D3D12_ROOT_PARAMETER1 extra {};
    extra.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    extra.Constants = { 0, 31, 8 };
    extra.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    result.constantsSlot = static_cast<UINT>(parameters.size());
    parameters.push_back(extra);
    auto append = [&](D3D12_ROOT_PARAMETER_TYPE type, UINT reg, D3D12_SHADER_VISIBILITY visibility)
    {
        extra = {};
        extra.ParameterType = type;
        extra.Descriptor = { reg, 31, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE };
        extra.ShaderVisibility = visibility;
        const auto slot = static_cast<UINT>(parameters.size());
        parameters.push_back(extra);
        return slot;
    };
    result.previousSlot = append(D3D12_ROOT_PARAMETER_TYPE_SRV, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    result.currentSlot = append(D3D12_ROOT_PARAMETER_TYPE_UAV, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    result.materialSlot = append(D3D12_ROOT_PARAMETER_TYPE_CBV, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    result.captureSlot = append(D3D12_ROOT_PARAMETER_TYPE_UAV, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    if (layout == GeometryLayout::PerInstance)
        result.instanceSlot = append(D3D12_ROOT_PARAMETER_TYPE_SRV, 1, D3D12_SHADER_VISIBILITY_ALL);
    result.dwords = cost + extraCost;
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned {};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned.Desc_1_1 = desc;
    ComPtr<ID3DBlob> blob, messages;
    hr = D3D12SerializeVersionedRootSignature(&versioned, &blob, &messages);
    if (FAILED(hr))
    {
        error = messages
                    ? std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize())
                    : "Cannot serialize extended root";
        return hr;
    }
    hr = device->CreateRootSignature(nodeMask, blob->GetBufferPointer(), blob->GetBufferSize(),
                                     IID_PPV_ARGS(&result.extended));
    if (FAILED(hr))
        return reject(error, "Cannot create extended root", hr);
    result.original = identity;
    output = std::move(result);
    return S_OK;
}

struct GeometryCompiler::Impl
{
    HMODULE module = nullptr;
    ComPtr<IDxcLibrary> library;
    ComPtr<IDxcCompiler> compiler;
    ComPtr<IDxcAssembler> assembler;
    ComPtr<IDxcValidator> validator;
    HRESULT status = E_FAIL;
    explicit Impl(const std::filesystem::path& path)
    {
        if (!path.is_absolute())
            return;
        module =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module)
            return;
        const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
        if (!create)
            return;
        if (FAILED(status = create(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) ||
            FAILED(status = create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) ||
            FAILED(status = create(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler))))
            return;
        status = create(CLSID_DxcValidator, IID_PPV_ARGS(&validator));
    }
    ~Impl()
    {
        validator.Reset();
        assembler.Reset();
        compiler.Reset();
        library.Reset();
        if (module)
            FreeLibrary(module);
    }
    HRESULT operation(IDxcOperationResult* op, ComPtr<IDxcBlob>& output, std::string& error)
    {
        HRESULT hr = E_FAIL;
        if (!op || FAILED(op->GetStatus(&hr)))
            return reject(error, "Missing DXC operation result", E_FAIL);
        if (FAILED(hr))
        {
            ComPtr<IDxcBlobEncoding> messages;
            op->GetErrorBuffer(&messages);
            error = messages
                        ? std::string(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize())
                        : "DXC operation rejected";
            return hr;
        }
        return op->GetResult(&output);
    }
    HRESULT rewrite(D3D12_SHADER_BYTECODE input, bool vertex, MaterialSource source, MaterialDestination destination,
                    ComPtr<IDxcBlob>& output, std::string& error, unsigned& historyRegister, GeometryLayout layout,
                    MaterialMotionTarget target, const VertexConstantPair* capture = nullptr,
                    const VertexClipPair* clipPair = nullptr)
    {
        if (!input.pShaderBytecode || !input.BytecodeLength || input.BytecodeLength > 2 * 1024 * 1024)
            return reject(error, "Missing or oversized shader");
        ComPtr<IDxcBlobEncoding> bytes, disassembly;
        HRESULT hr = library->CreateBlobWithEncodingOnHeapCopy(input.pShaderBytecode,
                                                               static_cast<UINT32>(input.BytecodeLength), 0, &bytes);
        if (FAILED(hr) || FAILED(hr = compiler->Disassemble(bytes.Get(), &disassembly)))
            return reject(error, "Cannot disassemble shader", hr);
        const std::string_view text(static_cast<const char*>(disassembly->GetBufferPointer()),
                                    disassembly->GetBufferSize());
        auto rewritten =
            vertex ? RewriteVertexHistory(text, layout, capture, clipPair)
                   : RewriteMaterialMotion(text, source, destination, target, historyRegister, layout);
        if (!rewritten)
        {
            error = rewritten.error;
            return E_NOTIMPL;
        }
        if (vertex)
            historyRegister = rewritten.previousRegister;
        hr = library->CreateBlobWithEncodingOnHeapCopy(rewritten.assembly.data(),
                                                       static_cast<UINT32>(rewritten.assembly.size()), CP_UTF8, &bytes);
        ComPtr<IDxcOperationResult> op;
        if (FAILED(hr) || FAILED(hr = assembler->AssembleToContainer(bytes.Get(), &op)))
            return reject(error, "Cannot assemble modified shader", hr);
        if (FAILED(hr = operation(op.Get(), output, error)))
            return hr;
        op.Reset();
        if (FAILED(hr = validator->Validate(output.Get(), DxcValidatorFlags_InPlaceEdit, &op)))
            return reject(error, "Cannot validate modified shader", hr);
        ComPtr<IDxcBlob> validated;
        return operation(op.Get(), validated, error);
    }
};
GeometryCompiler::GeometryCompiler(const std::filesystem::path& path) : implementation(std::make_unique<Impl>(path)) {}
GeometryCompiler::~GeometryCompiler() = default;
HRESULT GeometryCompiler::create(ID3D12Device* device, const GeometryRoot& root,
                                 const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                 ComPtr<ID3D12PipelineState>& output, std::string& error)
{
    return createTarget(device, root, original, output, error, MaterialMotionTarget::OriginalColorAndCapture);
}
HRESULT GeometryCompiler::createCoverage(ID3D12Device* device, const GeometryRoot& root,
                                         const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                         ComPtr<ID3D12PipelineState>& output, std::string& error)
{
    if (root.layout != GeometryLayout::PerInstance)
        return reject(error, "Object coverage requires per-instance identity mapping");
    return createTarget(device, root, original, output, error, MaterialMotionTarget::OriginalColorAndCoverage);
}
HRESULT GeometryCompiler::createCoverageAudit(ID3D12Device* device, const GeometryRoot& root,
                                              const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                              ComPtr<ID3D12PipelineState>& output, std::string& error,
                                              const VertexConstantPair* capture)
{
    if (root.layout != GeometryLayout::PerInstance)
        return reject(error, "Coverage audit requires per-instance identity mapping");
    return createTarget(device, root, original, output, error, MaterialMotionTarget::OriginalColorAndCoverageAudit,
                        false, capture);
}
HRESULT GeometryCompiler::createVertexCapture(ID3D12Device* device, const GeometryRoot& root,
                                              const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                              ComPtr<ID3D12PipelineState>& output, std::string& error,
                                              const VertexConstantPair* capture, const VertexClipPair* clipPair)
{
    return createTarget(device, root, original, output, error,
                        MaterialMotionTarget::OriginalColorAndCapture, true, capture, clipPair);
}
HRESULT GeometryCompiler::createTarget(ID3D12Device* device, const GeometryRoot& root,
                                       const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                       ComPtr<ID3D12PipelineState>& output, std::string& error,
                                       MaterialMotionTarget target, bool vertexOnly, const VertexConstantPair* capture,
                                       const VertexClipPair* clipPair)
{
    error.clear();
    if (FAILED(implementation->status))
        return reject(error, "DXC is unavailable", implementation->status);
    D3D12_BLEND_DESC validatedBlend {};
    if (!device || !root.extended || !root.original || root.original.Get() != original.pRootSignature ||
        !original.NumRenderTargets || original.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ||
        original.SampleDesc.Count != 1 || original.GS.BytecodeLength || original.HS.BytecodeLength ||
        original.DS.BytecodeLength || original.StreamOutput.NumEntries || original.StreamOutput.NumStrides ||
        (!vertexOnly && !readOnly(original.DepthStencilState)) ||
        original.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ||
        (!vertexOnly && !tryMaterialCaptureBlend(original.BlendState, MaterialCapture::SourceColor, validatedBlend)))
        return reject(error, "Unsupported original pipeline, blend, depth/stencil or geometry");
    // Vertex capture replaces the original draw once. Its unmodified PS and
    // depth/stencil/blend state retain native writes; material capture still
    // requires its separate read-only-depth and supported-blend contract.
    D3D12_FEATURE_DATA_D3D12_OPTIONS options {};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    if (!vertexOnly && (FAILED(hr) || !options.ROVsSupported))
        return reject(error, "Rasterizer-ordered views unavailable", FAILED(hr) ? hr : E_NOTIMPL);
    const auto& blend = original.BlendState.RenderTarget[0];
    const auto source = blend.SrcBlend == D3D12_BLEND_ZERO  ? MaterialSource::Zero
                        : blend.SrcBlend == D3D12_BLEND_ONE ? MaterialSource::One
                                                            : MaterialSource::Alpha;
    const auto destination = blend.DestBlend == D3D12_BLEND_ZERO            ? MaterialDestination::Zero
                             : blend.DestBlend == D3D12_BLEND_ONE           ? MaterialDestination::One
                             : blend.DestBlend == D3D12_BLEND_SRC_ALPHA     ? MaterialDestination::Alpha
                             : blend.DestBlend == D3D12_BLEND_INV_SRC_ALPHA ? MaterialDestination::OneMinusAlpha
                                                                            : MaterialDestination::SecondSourceRgb;
    ComPtr<IDxcBlob> vs, ps;
    unsigned historyRegister = UINT32_MAX;
    if (FAILED(hr = implementation->rewrite(original.VS, true, source, destination, vs, error, historyRegister,
                                            root.layout, target, capture, clipPair)) ||
        (!vertexOnly && FAILED(hr = implementation->rewrite(original.PS, false, source, destination, ps, error, historyRegister,
                                            root.layout, target))))
        return hr;
    // Same-draw capture retains every original export/attachment. Extra MRT
    // slots do not turn the RT0 material equation into a single-target PSO.
    auto modified = original;
    modified.pRootSignature = root.extended.Get();
    modified.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    if (!vertexOnly) modified.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    modified.CachedPSO = {};
    ComPtr<ID3D12PipelineState> result;
    hr = device->CreateGraphicsPipelineState(&modified, IID_PPV_ARGS(&result));
    if (FAILED(hr))
        return reject(error, "Extended shader/root pipeline rejected by D3D12", hr);
    output = std::move(result);
    return S_OK;
}
} // namespace GlassFg
