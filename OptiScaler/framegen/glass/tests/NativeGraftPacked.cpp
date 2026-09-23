// Packed native-graft rewrite on one exported Cyberpunk graft and a paired
// original pixel shader. Offline compiler/validator check: no device, no game.
// Usage: NativeGraftPacked dxcompiler.dll module-dir original-vs.dxbc ps.dxbc current previous
// module-dir must contain Glass/grafts written by tools/export_native_grafts.py.
#include "pch.h"
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
#include "../DxilVertexHistory.h"
#include "../NativeGraftCatalog.h"
using Microsoft::WRL::ComPtr;

namespace
{
std::filesystem::path moduleDirectory;
void require(bool ok, const std::string& what)
{
    if (!ok)
        throw std::runtime_error(what);
}
std::vector<char> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "Cannot read " + path.string());
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}
struct Dxc
{
    ComPtr<IDxcLibrary> library;
    ComPtr<IDxcCompiler> compiler;
    ComPtr<IDxcAssembler> assembler;
    ComPtr<IDxcValidator> validator;
    std::string disassemble(const void* bytes, size_t size)
    {
        ComPtr<IDxcBlobEncoding> blob, text;
        require(SUCCEEDED(library->CreateBlobWithEncodingOnHeapCopy(bytes, UINT32(size), 0, &blob)) &&
                    SUCCEEDED(compiler->Disassemble(blob.Get(), &text)),
                "Disassembly failed");
        return { static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize() };
    }
    // Same assemble + in-place validate sequence as GeometryCompiler::Impl::rewrite.
    void assembleAndValidate(const std::string& assembly, const char* stage)
    {
        ComPtr<IDxcBlobEncoding> source;
        ComPtr<IDxcOperationResult> op;
        ComPtr<IDxcBlob> container;
        HRESULT status = E_FAIL;
        require(SUCCEEDED(library->CreateBlobWithEncodingOnHeapCopy(assembly.data(), UINT32(assembly.size()), CP_UTF8,
                                                                     &source)) &&
                    SUCCEEDED(assembler->AssembleToContainer(source.Get(), &op)) && SUCCEEDED(op->GetStatus(&status)),
                std::string(stage) + " assembly call failed");
        auto errors = [&](const char* phase) {
            ComPtr<IDxcBlobEncoding> messages;
            op->GetErrorBuffer(&messages);
            return std::string(stage) + " " + phase + " rejected: " +
                   (messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()),
                                           messages->GetBufferSize())
                             : std::string());
        };
        require(SUCCEEDED(status), errors("assembly"));
        require(SUCCEEDED(op->GetResult(&container)), "No assembled container");
        op.Reset();
        require(SUCCEEDED(validator->Validate(container.Get(), DxcValidatorFlags_InPlaceEdit, &op)) &&
                    SUCCEEDED(op->GetStatus(&status)),
                std::string(stage) + " validation call failed");
        require(SUCCEEDED(status), errors("validation"));
    }
};
// Row of the one signature element with this semantic (index 0), or -1.
int semanticRow(const std::string& assembly, const std::string& semantic)
{
    const std::regex element("= !\\{i32 \\d+, !\"" + semantic +
                             "\", i8 \\d+, i8 \\d+, ![0-9]+, i8 \\d+, i32 1, i8 \\d, i32 (\\d+), i8 0,");
    int row = -1, matches = 0;
    for (std::sregex_iterator it(assembly.begin(), assembly.end(), element), end; it != end; ++it, ++matches)
        row = std::stoi((*it)[1].str());
    require(matches <= 1, "Ambiguous signature element " + semantic);
    return row;
}
} // namespace

namespace Util
{
std::filesystem::path DllPath() { return moduleDirectory / L"OptiScaler.dll"; }
} // namespace Util

int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 7, "NativeGraftPacked dxcompiler.dll module-dir original-vs.dxbc ps.dxbc current previous");
        moduleDirectory = argv[2];
        const unsigned expectedCurrent = std::stoul(argv[5]), expectedPrevious = std::stoul(argv[6]);
        const auto dll = LoadLibraryExW(argv[1], nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(dll != nullptr, "Cannot load dxcompiler");
        const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance"));
        require(create != nullptr, "No DxcCreateInstance");
        Dxc dxc;
        require(SUCCEEDED(create(CLSID_DxcLibrary, IID_PPV_ARGS(&dxc.library))) &&
                    SUCCEEDED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc.compiler))) &&
                    SUCCEEDED(create(CLSID_DxcAssembler, IID_PPV_ARGS(&dxc.assembler))) &&
                    SUCCEEDED(create(CLSID_DxcValidator, IID_PPV_ARGS(&dxc.validator))),
                "DXC instances unavailable");

        // Catalog: the original VS container hash selects the graft; the pixel
        // shader bytes are a miss.
        const auto originalVs = readFile(argv[3]);
        const auto pixel = readFile(argv[4]);
        const auto graft = GlassFg::FindNativeGraft(originalVs.data(), originalVs.size());
        require(graft.has_value(), "Catalog miss for the original VS");
        require(graft->currentOutput == expectedCurrent && graft->previousOutput == expectedPrevious,
                "Catalog output ids differ from index.json");
        const auto again = GlassFg::FindNativeGraft(originalVs.data(), originalVs.size());
        require(again && again->bytes == graft->bytes, "Second lookup did not reuse the loaded graft");
        require(!GlassFg::FindNativeGraft(pixel.data(), pixel.size()), "Pixel shader bytes matched a graft");
        std::printf("catalog grafts=%zu hit current=%u previous=%u class=%u bytes=%zu\n", GlassFg::NativeGraftCount(),
                    graft->currentOutput, graft->previousOutput, graft->supplyClass, graft->size);

        // Vertex stage in native-previous mode.
        const GlassFg::VertexClipPair outputs { graft->currentOutput, graft->previousOutput };
        const auto vertex = GlassFg::RewriteVertexHistory(dxc.disassemble(graft->bytes, graft->size),
                                                          GlassFg::GeometryLayout::PerInstance, nullptr, nullptr,
                                                          nullptr, false, false, &outputs);
        require(bool(vertex), "Vertex rewrite failed: " + vertex.error);
        const auto& vs = vertex.assembly;
        require(vs.find("GlassHistory") == std::string::npos && vs.find("%glass.srv") == std::string::npos,
                "Native VS declares or loads GlassHistory");
        require(vs.find("GlassNext") == std::string::npos && vs.find("@dx.op.bufferStore") == std::string::npos,
                "Native VS declares or stores GlassNext");
        require(vs.find("GLASS_PREVIOUS") == std::string::npos && vs.find("GLASS_CAPTURE_DELTA") == std::string::npos,
                "Native VS exports a history varying");
        require(vs.find("!\"GlassInstances\", i32 31, i32 1,") != std::string::npos &&
                    vs.find("!\"GlassConstants\", i32 31, i32 0,") != std::string::npos,
                "Native VS lost GlassInstances t1 / GlassConstants b0");
        require(vertex.missingRegister == vertex.previousRegister, "Native VS missing flag must start the rows");
        dxc.assembleAndValidate(vs, "vertex");

        // Pixel stage: packed native material pair from the linked graft rows.
        GlassFg::NativeClipInputs inputs { graft->currentOutput, graft->previousOutput, false, true };
        inputs.linked = vertex.nativeVaryings;
        const auto pixelText = dxc.disassemble(pixel.data(), pixel.size());
        auto material = GlassFg::RewriteMaterialMotion(
            pixelText, GlassFg::MaterialSource::Alpha, GlassFg::MaterialDestination::OneMinusAlpha,
            GlassFg::MaterialMotionTarget::OriginalColorAndPackedMotion, vertex.previousRegister,
            GlassFg::GeometryLayout::PerInstance, &inputs, true, false);
        const char* variant = "alpha";
        if (!material)
        {
            std::printf("material equation rejected (%s); coverage-only variant\n", material.error.c_str());
            material = GlassFg::RewriteMaterialMotion(
                pixelText, GlassFg::MaterialSource::Zero, GlassFg::MaterialDestination::CoverageOnly,
                GlassFg::MaterialMotionTarget::OriginalColorAndPackedMotion, vertex.previousRegister,
                GlassFg::GeometryLayout::PerInstance, &inputs, true, false);
            variant = "coverage";
        }
        require(bool(material), "Pixel rewrite failed: " + material.error);
        const auto& ps = material.assembly;
        require(ps.find("%glass.j") == std::string::npos && ps.find("%glass.captureDelta") == std::string::npos,
                "Packed native PS adds a jitter or capture-delta term");
        require(ps.find("GLASS_PREVIOUS") == std::string::npos, "Packed native PS declares GLASS_PREVIOUS");
        // Linkage: every varying the rewrite introduced sits at the same row
        // and semantic in both stages.
        auto name = [](const std::string& quoted) { return quoted.substr(2, quoted.size() - 3); };
        for (const auto& semantic : { name(vertex.nativeVaryings[0].semantic), name(vertex.nativeVaryings[1].semantic),
                                      std::string("GLASS_HISTORY_MISSING"), std::string("GLASS_OBJECT_INDEX") })
        {
            const int vsRow = semanticRow(vs, semantic), psRow = semanticRow(ps, semantic);
            std::printf("link %s vs_row=%d ps_row=%d\n", semantic.c_str(), vsRow, psRow);
            require(vsRow >= 0 && vsRow == psRow, "Varying " + semantic + " does not link");
        }
        require(semanticRow(ps, name(vertex.nativeVaryings[0].semantic)) == int(vertex.nativeVaryings[0].row) &&
                    semanticRow(ps, name(vertex.nativeVaryings[1].semantic)) == int(vertex.nativeVaryings[1].row),
                "Clip varying rows differ from the graft output rows");
        dxc.assembleAndValidate(ps, "pixel");
        std::printf("native graft packed rewrite passed (%s variant, missing_row=%u)\n", variant,
                    vertex.missingRegister);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
