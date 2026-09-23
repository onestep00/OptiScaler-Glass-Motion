// Packed native-graft rewrite on one exported Cyberpunk graft and a paired
// original pixel shader. Offline compiler/validator check: no device, no game.
// Usage: NativeGraftPacked dxcompiler.dll module-dir original-vs.dxbc ps.dxbc current previous
//        camera-current camera-previous
// module-dir must contain Glass/grafts written by tools/export_native_grafts.py.
// current and previous are "none" for a camera-only record (a VS without a
// native current-position twin): no root graft, only the camera variant.
#include "pch.h"
#include <dxcapi.h>
#include <wrl/client.h>
#include <algorithm>
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
// Row and first semantic index of the one signature element with this
// semantic, or {-1, -1}.
struct SignatureElement
{
    int row = -1, index = -1;
};
SignatureElement signatureElement(const std::string& assembly, const std::string& semantic)
{
    const std::regex element("= !\\{i32 \\d+, !\"" + semantic +
                             "\", i8 \\d+, i8 \\d+, (![0-9]+), i8 \\d+, i32 1, i8 \\d, i32 (\\d+), i8 0,");
    SignatureElement value;
    std::string indexNode;
    int matches = 0;
    for (std::sregex_iterator it(assembly.begin(), assembly.end(), element), end; it != end; ++it, ++matches)
    {
        indexNode = (*it)[1].str();
        value.row = std::stoi((*it)[2].str());
    }
    require(matches <= 1, "Ambiguous signature element " + semantic);
    if (matches == 0)
        return value;
    // The element references its semantic index list, e.g. !17 = !{i32 7}.
    std::smatch list;
    require(std::regex_search(assembly, list, std::regex("\n" + indexNode + " = !\\{i32 (\\d+)[,}]")),
            "No semantic index list " + indexNode + " for " + semantic);
    value.index = std::stoi(list[1].str());
    return value;
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
        require(argc == 9, "NativeGraftPacked dxcompiler.dll module-dir original-vs.dxbc ps.dxbc current previous "
                           "camera-current camera-previous");
        moduleDirectory = argv[2];
        const bool cameraOnly = std::wstring(argv[5]) == L"none" && std::wstring(argv[6]) == L"none";
        const unsigned expectedCurrent = cameraOnly ? 0 : std::stoul(argv[5]);
        const unsigned expectedPrevious = cameraOnly ? 0 : std::stoul(argv[6]);
        const unsigned expectedCameraCurrent = std::stoul(argv[7]), expectedCameraPrevious = std::stoul(argv[8]);
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
        require(cameraOnly == !graft->bytes && cameraOnly == !graft->size,
                cameraOnly ? "Camera-only record returned root graft bytes" : "Catalog hit has no root graft");
        const auto again = GlassFg::FindNativeGraft(originalVs.data(), originalVs.size());
        require(again && again->bytes == graft->bytes, "Second lookup did not reuse the loaded graft");
        require(!GlassFg::FindNativeGraft(pixel.data(), pixel.size()), "Pixel shader bytes matched a graft");
        std::printf("catalog grafts=%zu hit current=%u previous=%u class=%u bytes=%zu\n", GlassFg::NativeGraftCount(),
                    graft->currentOutput, graft->previousOutput, graft->supplyClass, graft->size);

        // Constant-buffer rows a VS loads through the handle bound to register
        // b<binding> (space 0), from its disassembly.
        auto loadedRows = [](const std::string& assembly, unsigned binding) {
            std::vector<unsigned> rows;
            const std::regex handle("(%[0-9A-Za-z_.]+) = call %dx.types.Handle @dx.op.createHandle\\(i32 57, i8 2, "
                                    "i32 \\d+, i32 " + std::to_string(binding) + ", i1 false\\)");
            for (std::sregex_iterator it(assembly.begin(), assembly.end(), handle), end; it != end; ++it)
            {
                const std::regex load("@dx.op.cbufferLoadLegacy.\\w+\\(i32 59, %dx.types.Handle " + (*it)[1].str() +
                                      ", i32 (\\d+)\\)");
                for (std::sregex_iterator row(assembly.begin(), assembly.end(), load); row != end; ++row)
                    rows.push_back(std::stoul((*row)[1].str()));
            }
            return rows;
        };
        auto loads = [](const std::vector<unsigned>& rows, unsigned first, unsigned count) {
            unsigned found = 0;
            for (unsigned row = first; row < first + count; ++row)
                found += std::find(rows.begin(), rows.end(), row) != rows.end() ? 1 : 0;
            return found;
        };
        // Packed rewrite of one grafted VS and the paired PS: native-previous VS
        // mode, linked graft rows, DXC assembly and validation of both stages.
        const auto pixelText = dxc.disassemble(pixel.data(), pixel.size());
        auto rewritePacked = [&](const char* label, const void* bytes, std::size_t size, unsigned current,
                                 unsigned previous) {
            const GlassFg::VertexClipPair outputs { current, previous };
            const auto vertex = GlassFg::RewriteVertexHistory(dxc.disassemble(bytes, size),
                                                              GlassFg::GeometryLayout::PerInstance, nullptr, nullptr,
                                                              nullptr, false, false, &outputs);
            require(bool(vertex), std::string(label) + " vertex rewrite failed: " + vertex.error);
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
            dxc.assembleAndValidate(vs, (std::string(label) + " vertex").c_str());

            // Pixel stage: packed native material pair from the linked graft rows.
            GlassFg::NativeClipInputs inputs { current, previous, false, true };
            inputs.linked = vertex.nativeVaryings;
            auto material = GlassFg::RewriteMaterialMotion(
                pixelText, GlassFg::MaterialSource::Alpha, GlassFg::MaterialDestination::OneMinusAlpha,
                GlassFg::MaterialMotionTarget::OriginalColorAndPackedMotion, vertex.previousRegister,
                GlassFg::GeometryLayout::PerInstance, &inputs, true, false);
            const char* variant = "alpha";
            if (!material)
            {
                std::printf("%s material equation rejected (%s); coverage-only variant\n", label,
                            material.error.c_str());
                material = GlassFg::RewriteMaterialMotion(
                    pixelText, GlassFg::MaterialSource::Zero, GlassFg::MaterialDestination::CoverageOnly,
                    GlassFg::MaterialMotionTarget::OriginalColorAndPackedMotion, vertex.previousRegister,
                    GlassFg::GeometryLayout::PerInstance, &inputs, true, false);
                variant = "coverage";
            }
            require(bool(material), std::string(label) + " pixel rewrite failed: " + material.error);
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
                const auto vsElement = signatureElement(vs, semantic), psElement = signatureElement(ps, semantic);
                std::printf("%s link %s vs_row=%d ps_row=%d vs_index=%d ps_index=%d\n", label, semantic.c_str(),
                            vsElement.row, psElement.row, vsElement.index, psElement.index);
                require(vsElement.row >= 0 && vsElement.row == psElement.row, "Varying " + semantic + " does not link");
                require(vsElement.index == psElement.index, "Varying " + semantic + " semantic index differs");
            }
            for (const auto& varying : vertex.nativeVaryings)
            {
                const auto vsElement = signatureElement(vs, name(varying.semantic));
                const auto psElement = signatureElement(ps, name(varying.semantic));
                require(psElement.row == int(varying.row), "Clip varying row differs from the graft output row");
                require(vsElement.index == int(varying.semanticIndex) && psElement.index == int(varying.semanticIndex),
                        "Clip varying semantic index differs from the graft output");
            }
            dxc.assembleAndValidate(ps, (std::string(label) + " pixel").c_str());
            std::printf("%s graft packed rewrite passed (%s variant, missing_row=%u)\n", label, variant,
                        vertex.missingRegister);
        };

        // Root graft: previous clip reads the relocated MotionMatrix rows b7[24..26].
        if (!cameraOnly)
        {
            const auto rootText = dxc.disassemble(graft->bytes, graft->size);
            require(loads(loadedRows(rootText, 7), 24, 3) == 3, "Root graft does not read MotionMatrix b7[24..26]");
            rewritePacked("root", graft->bytes, graft->size, graft->currentOutput, graft->previousOutput);
        }

        // Camera-only variant of the same original VS (array draws of a root
        // graft, every draw of a camera-only record): previous clip is the
        // native previous view-projection (b1 rows 16..19, or 12..15 in the
        // forward layout) on the VS's own current world position, with no
        // MotionMatrix read.
        require(graft->cameraBytes && graft->cameraSize, "Catalog hit has no camera-only variant");
        require(graft->cameraCurrentOutput == expectedCameraCurrent &&
                    graft->cameraPreviousOutput == expectedCameraPrevious,
                "Catalog camera output ids differ from index.json");
        require(again->cameraBytes == graft->cameraBytes, "Second lookup did not reuse the loaded camera variant");
        const auto cameraText = dxc.disassemble(graft->cameraBytes, graft->cameraSize);
        require(loads(loadedRows(cameraText, 7), 24, 3) == 0, "Camera variant reads MotionMatrix b7[24..26]");
        const auto cameraRows = loadedRows(cameraText, 1);
        require(loads(cameraRows, 16, 4) == 4 || loads(cameraRows, 12, 4) == 4,
                "Camera variant lacks the native previous camera rows b1[16..19] / b1[12..15]");
        std::printf("camera variant bytes=%zu current=%u previous=%u\n", graft->cameraSize, graft->cameraCurrentOutput,
                    graft->cameraPreviousOutput);
        rewritePacked("camera", graft->cameraBytes, graft->cameraSize, graft->cameraCurrentOutput,
                      graft->cameraPreviousOutput);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
