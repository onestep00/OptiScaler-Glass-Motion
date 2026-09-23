#include "pch.h"
#include "GeometryPipeline.h"
#include "DxilVertexHistory.h"
#include "GlassControls.h"
#include "MaterialCaptureBlend.h"
#include <dxcapi.h>
#include <atomic>
#include <array>
#include <stdexcept>

namespace GlassFg
{
using Microsoft::WRL::ComPtr;
namespace
{
// Crash forensics: the marker exists only while one of our rewritten pipelines
// is being created. A driver reset during pipeline creation leaves the file
// behind, which separates "our PSO" from "our compose" in the crash evidence.
std::wstring pipelineMarkerPath()
{
    wchar_t path[MAX_PATH] {};
    const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length == MAX_PATH)
        return {};
    std::wstring text(path, length);
    const auto cut = text.find_last_of(L"\\/");
    if (cut == std::wstring::npos)
        return {};
    text.resize(cut);
    return text + L"\\Glass\\glass-pso.pending";
}
void writePipelineMarker(const char* stage, unsigned target, std::uint64_t key) noexcept
{
    const auto path = pipelineMarkerPath();
    if (path.empty())
        return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
        return;
    std::fprintf(file, "stage=%s target=%u key=%016llx tick=%llu\n", stage, target,
                 static_cast<unsigned long long>(key), static_cast<unsigned long long>(GetTickCount64()));
    std::fclose(file);
}
void clearPipelineMarker() noexcept
{
    const auto path = pipelineMarkerPath();
    if (!path.empty())
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
}
std::uint64_t pipelineKey(const ComPtr<IDxcBlob>& vs, const ComPtr<IDxcBlob>& ps) noexcept
{
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (auto* blob : { vs.Get(), ps.Get() })
        if (blob)
        {
            const auto* bytes = static_cast<const unsigned char*>(blob->GetBufferPointer());
            for (SIZE_T i = 0; i < blob->GetBufferSize(); ++i)
                hash = (hash ^ bytes[i]) * 0x100000001b3ull;
        }
    return hash;
}
} // namespace
// Packed variants that fell back to coverage-only capture because the material
// exports could not be read safely. Diagnostics only.
std::atomic<std::uint64_t> packedCoverageFallbacks { 0 };
std::uint64_t ReadPackedCoverageFallbackCount() noexcept
{
    return packedCoverageFallbacks.load(std::memory_order_relaxed);
}
// Packed pipelines that compiled without the capture constant pair because the
// audited b1/space0 block was absent, ambiguous or oversized. Those pipelines
// keep the pre-pair tag payload and no capture delta.
std::atomic<std::uint64_t> packedCaptureFallbacks { 0 };
std::uint64_t ReadPackedCaptureFallbackCount() noexcept
{
    return packedCaptureFallbacks.load(std::memory_order_relaxed);
}
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
                    const VertexClipPair* clipPair = nullptr, const NativeClipInputs* nativeInputs = nullptr,
                    const VertexInputPair* inputPair = nullptr, bool captureDelta = false,
                    bool markMissingDelta = false)
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
            vertex ? RewriteVertexHistory(text, layout, capture, clipPair, inputPair, captureDelta, markMissingDelta)
                   : RewriteMaterialMotion(text, source, destination, target, historyRegister, layout, nativeInputs,
                                           target == MaterialMotionTarget::OriginalColorAndPackedMotion, captureDelta);
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
HRESULT GeometryCompiler::createPackedMotion(ID3D12Device* device, const GeometryRoot& root,
                                             const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                             ComPtr<ID3D12PipelineState>& output, std::string& error,
                                             const VertexConstantPair* capture, bool* pairMissing)
{
    if (pairMissing)
        *pairMissing = false;
    if (root.layout != GeometryLayout::PerInstance)
        return reject(error, "Packed motion requires per-instance identity mapping");
    if (capture)
    {
        const auto status = createTarget(device, root, original, output, error,
                                         MaterialMotionTarget::OriginalColorAndPackedMotion, false, capture);
        if (SUCCEEDED(status))
            return status;
        // R6: the audited constant block is not present in every transparent
        // vertex shader. Recompile without the pair so the pipeline keeps its
        // original tag payload and the pixel shader drops the delta term. This
        // is the pre-pair behaviour, counted for the live log and reported to
        // the caller: a pair-less record carries the raw jittered difference,
        // which the delivery path must not substitute (F-01).
        ++packedCaptureFallbacks;
        if (pairMissing)
            *pairMissing = true;
        output.Reset();
        error.clear();
    }
    return createTarget(device, root, original, output, error,
                        MaterialMotionTarget::OriginalColorAndPackedMotion);
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
                                              const VertexConstantPair* capture, const VertexClipPair* clipPair,
                                              const VertexInputPair* inputPair)
{
    return createTarget(device, root, original, output, error,
                        MaterialMotionTarget::OriginalColorAndCapture, true, capture, clipPair, nullptr, inputPair);
}
HRESULT GeometryCompiler::createNativeMotionCapture(ID3D12Device* device, const GeometryRoot& root,
                                       const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                       ComPtr<ID3D12PipelineState>& output, std::string& error,
                                       const NativeClipInputs& inputs)
{
    return createTarget(device, root, original, output, error,
                        MaterialMotionTarget::OriginalColorAndCapture, false, nullptr, nullptr, &inputs);
}
HRESULT GeometryCompiler::createTarget(ID3D12Device* device, const GeometryRoot& root,
                                       const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                       ComPtr<ID3D12PipelineState>& output, std::string& error,
                                       MaterialMotionTarget target, bool vertexOnly, const VertexConstantPair* capture,
                                       const VertexClipPair* clipPair, const NativeClipInputs* nativeInputs,
                                       const VertexInputPair* inputPair)
{
    error.clear();
    if (FAILED(implementation->status))
        return reject(error, "DXC is unavailable", implementation->status);
    D3D12_BLEND_DESC validatedBlend {};
    bool nativeBlend = !original.BlendState.AlphaToCoverageEnable;
    for (UINT i = 0; i < std::min(original.NumRenderTargets, UINT(D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)); ++i)
    {
        const auto& rt = original.BlendState.RenderTarget[original.BlendState.IndependentBlendEnable ? i : 0];
        nativeBlend = nativeBlend && !rt.BlendEnable && !rt.LogicOpEnable;
    }
    const bool packedMotion = target == MaterialMotionTarget::OriginalColorAndPackedMotion;
    const bool coverageAudit = target == MaterialMotionTarget::OriginalColorAndCoverageAudit;
    const bool depthCoverage = coverageAudit && nativeBlend;
    const bool nativeOpaque = nativeInputs && !nativeInputs->material;
    const bool material = !vertexOnly && !nativeOpaque && !depthCoverage;
    // Diagnostic opaque probe (GlassFG/OpaqueProbe, default off). The cache half
    // of the flag admits the depth-writing and unblended pipelines the product
    // gate refuses; the compiler half here relaxes exactly the two guards those
    // pipelines would otherwise hit and cannot drop:
    //   * read-only depth/stencil - the packed variant is an in-place rewrite of
    //     the same draw with the same depth/stencil state, and the rewritten
    //     pixel shader keeps every original store and adds no discard (the
    //     rewrite helper removes the temporary discard and colour stores it
    //     generates), so the depth write reaches the buffer exactly as the
    //     engine's own pipeline would. The packed disassembly fixture pins that.
    //   * the blend equation - an unblended target writes the source colour
    //     directly, so the surface is fully opaque: transmission 0, weight 255.
    //     Without this the record would be classified uncovered or skipped
    //     entirely depending on the ignored blend factors the game left in the
    //     description.
    // A pixel shader that exports SV_Depth is still rejected by the material
    // output analysis and recovers, as today, as a coverage-only variant that
    // keeps the original colour and depth exports. Probe-off sessions take the
    // original paths unchanged.
    const bool opaqueProbe = packedMotion && OpaqueProbeEnabled();
    const bool probeOpaque = opaqueProbe && nativeBlend;
    if (depthCoverage) target = MaterialMotionTarget::OriginalColorAndDepthCoverageAudit;
    if (!device || !root.extended || !root.original || root.original.Get() != original.pRootSignature)
        return reject(error, "Missing device or mismatched extended root");
    if (!original.NumRenderTargets || original.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
        return reject(error, "Unsupported render-target count");
    if (original.SampleDesc.Count != 1)
        return reject(error, "Multisampled pipeline unsupported");
    if (original.GS.BytecodeLength || original.HS.BytecodeLength || original.DS.BytecodeLength ||
        original.StreamOutput.NumEntries || original.StreamOutput.NumStrides)
        return reject(error, "Additional graphics stages or stream output unsupported");
    if (material && !readOnly(original.DepthStencilState) && !opaqueProbe)
        return reject(error, "Material capture requires read-only depth/stencil");
    if (original.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
        return reject(error, "Non-triangle pipeline unsupported");
    if (nativeOpaque && !nativeBlend)
        return reject(error, "Native opaque capture requires unblended targets");
    const bool knownMaterialBlend = material &&
        tryMaterialCaptureBlend(original.BlendState, MaterialCapture::SourceColor, validatedBlend,
                                coverageAudit || packedMotion);
    // Packed capture never changes the application's attachments or blend state.
    // For destination-dependent equations, retain exact shader coverage/object
    // motion but use zero interior weight; the compositor may still apply full
    // object motion to the visible boundary without dragging the background.
    const auto& primaryBlend = original.BlendState.RenderTarget[0];
    const bool packedCoverageOnly = packedMotion && !knownMaterialBlend && primaryBlend.BlendEnable &&
                                    !primaryBlend.LogicOpEnable && primaryBlend.RenderTargetWriteMask;
    if (material && !knownMaterialBlend && !packedCoverageOnly && !probeOpaque)
        return reject(error, "Unsupported material blend equation");
    // Vertex capture replaces the original draw once. Its unmodified PS and
    // depth/stencil/blend state retain native writes. Unblended coverage audit
    // uses the no-discard native-output contract; blended material capture
    // still requires read-only depth and its supported blend equation.
    HRESULT hr = S_OK;
    if (packedMotion)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel { D3D_SHADER_MODEL_6_6 };
        hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));
        if (FAILED(hr) || shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6)
            return reject(error, "Shader Model 6.6 unavailable", FAILED(hr) ? hr : E_NOTIMPL);
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options {};
        hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options));
        if (FAILED(hr) || !options.Int64ShaderOps)
            return reject(error, "64-bit shader operations unavailable", FAILED(hr) ? hr : E_NOTIMPL);
    }
    else if (!vertexOnly)
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options {};
        hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
        if (FAILED(hr) || !options.ROVsSupported)
            return reject(error, "Rasterizer-ordered views unavailable", FAILED(hr) ? hr : E_NOTIMPL);
    }
    const auto& blend = original.BlendState.RenderTarget[0];
    // An unblended target under the probe is a fully opaque surface: the source
    // colour is written directly, so the record carries transmission 0 and both
    // the colour contribution and the coverage class are definite. The blend
    // factors of an unblended description are ignored by the device, so they
    // must not be read here.
    const auto source = probeOpaque ? MaterialSource::One : packedCoverageOnly ? MaterialSource::Zero : depthCoverage ? MaterialSource::One : nativeOpaque ? MaterialSource::Zero : blend.SrcBlend == D3D12_BLEND_ZERO  ? MaterialSource::Zero
                        : blend.SrcBlend == D3D12_BLEND_ONE ? MaterialSource::One
                                                            : MaterialSource::Alpha;
    const auto destination = probeOpaque ? MaterialDestination::Zero : packedCoverageOnly ? MaterialDestination::CoverageOnly : (nativeOpaque || depthCoverage) ? MaterialDestination::Zero : blend.DestBlend == D3D12_BLEND_ZERO            ? MaterialDestination::Zero
                             : blend.DestBlend == D3D12_BLEND_ONE           ? MaterialDestination::One
                             : blend.DestBlend == D3D12_BLEND_SRC_ALPHA     ? MaterialDestination::Alpha
                             : blend.DestBlend == D3D12_BLEND_INV_SRC_ALPHA ? MaterialDestination::OneMinusAlpha
                                                                             : MaterialDestination::SecondSourceRgb;
    ComPtr<IDxcBlob> vs, ps;
    unsigned historyRegister = UINT32_MAX;
    // The capture delta is a packed-path contract: the vertex stage writes the
    // frame-to-frame difference of the captured constant words and the linked
    // pixel stage adds it. Coverage-audit captures keep their old payload.
    const bool captureDelta = packedMotion && capture != nullptr;
    // Packed variants compiled without the audited pair (R6) still write the
    // record tag, and the next paired frame would read their untouched bytes
    // 24/28 as its predecessor jitter. Mark those words as absent instead.
    const bool markMissingDelta = packedMotion && capture == nullptr;
    if (FAILED(hr = implementation->rewrite(original.VS, true, source, destination, vs, error, historyRegister,
                                            root.layout, target, capture, clipPair, nullptr, inputPair,
                                            captureDelta, markMissingDelta)))
        return hr;
    if (!vertexOnly)
    {
        hr = implementation->rewrite(original.PS, false, source, destination, ps, error, historyRegister,
                                     root.layout, target, nullptr, nullptr, nativeInputs, nullptr, captureDelta);
        if (FAILED(hr) && packedMotion && destination != MaterialDestination::CoverageOnly)
        {
            // The material equation needs colour exports this pixel shader does
            // not expose (depth/stencil exports, branch-local stores or no
            // colour output at all). Recover as a coverage-only packed variant:
            // the boundary still receives the object's motion and depth while
            // the interior keeps the engine's own motion. The original exports,
            // discard and blend state stay untouched. Extra MRT slots above
            // zero are not a rejection reason; they are skipped and the
            // material pass keeps them.
            ps.Reset();
            error.clear();
            hr = implementation->rewrite(original.PS, false, MaterialSource::Zero,
                                         MaterialDestination::CoverageOnly, ps, error, historyRegister, root.layout,
                                         target, nullptr, nullptr, nativeInputs, nullptr, captureDelta);
            if (SUCCEEDED(hr))
                ++packedCoverageFallbacks;
        }
        if (FAILED(hr))
            return hr;
    }
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
