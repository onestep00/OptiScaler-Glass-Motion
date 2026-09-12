// Local compiler bridge. No game attachment, memory reads or rendering changes.
#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <sstream>
#include "../DxilVertexHistory.h"
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("HRESULT " + std::to_string((unsigned) hr));
}
static ComPtr<IDxcBlob> result(IDxcOperationResult* operation)
{
    ComPtr<IDxcBlobEncoding> errors;
    operation->GetErrorBuffer(&errors);
    if (errors && errors->GetBufferSize())
        fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    HRESULT status;
    check(operation->GetStatus(&status));
    check(status);
    ComPtr<IDxcBlob> bytes;
    check(operation->GetResult(&bytes));
    return bytes;
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc != 6 && argc != 7)
            throw std::runtime_error("tool dxcompiler.dll mode input output target [original VS for material linkage]");
        auto dll =
            LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll)
            throw std::runtime_error("Cannot load compiler");
        auto create = (DxcCreateInstanceProc) GetProcAddress(dll, "DxcCreateInstance");
        if (!create)
            throw std::runtime_error("No compiler factory");
        ComPtr<IDxcLibrary> library;
        check(create(CLSID_DxcLibrary, IID_PPV_ARGS(&library)));
        ComPtr<IDxcBlobEncoding> input;
        UINT32 codepage = CP_UTF8;
        check(library->CreateBlobFromFile(argv[3], &codepage, &input));
        ComPtr<IDxcBlob> output;
        std::wstring mode(argv[2]);
        const bool mapped = mode.ends_with(L"-mapped");
        if (mapped)
            mode.resize(mode.size() - 7);
        const auto layout = mapped ? GlassFg::GeometryLayout::PerInstance : GlassFg::GeometryLayout::Contiguous;
        const bool cameraCapture = mode == L"rewrite-camera";
        const bool pairCapture = mode == L"rewrite-pair";
        const bool inputCapture = mode == L"rewrite-input";
        GlassFg::VertexInputPair inputPair {};
        if (inputCapture)
        {
            std::wistringstream selection(argv[5]); wchar_t first = 0, second = 0;
            if (!(selection >> inputPair.input >> first >> inputPair.first >> second >> inputPair.second) ||
                first != L',' || second != L',' || selection.peek() != std::char_traits<wchar_t>::eof())
                throw std::runtime_error("Expected input-id,first-component,second-component");
        }
        GlassFg::VertexClipPair clipPair {};
        if (pairCapture)
        {
            const std::wstring selection(argv[5]);
            const auto comma = selection.find(L',');
            if (comma == std::wstring::npos || !comma || comma + 1 == selection.size() ||
                selection.find_first_not_of(L"0123456789,", 0) != std::wstring::npos ||
                selection.find(L',', comma + 1) != std::wstring::npos)
                throw std::runtime_error("Expected current-output-id,previous-output-id");
            clipPair = {std::stoul(selection.substr(0, comma)), std::stoul(selection.substr(comma + 1))};
        }
        if (cameraCapture || pairCapture || inputCapture)
            mode = L"rewrite";
        // Explicit recorded Cyberpunk diagnostic layout, not a generic camera detector.
        const GlassFg::VertexConstantPair camera { 0, 1, 848, 51 };
        if (mode == L"disassemble")
        {
            ComPtr<IDxcCompiler> compiler;
            check(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
            ComPtr<IDxcBlobEncoding> disassembly;
            check(compiler->Disassemble(input.Get(), &disassembly));
            output = disassembly;
        }
        else if (mode == L"assemble" || mode == L"rewrite" || mode == L"material" || mode == L"capture" ||
                 mode == L"coverage" || mode == L"native-motion")
        {
            if (mode == L"rewrite" || mode == L"material" || mode == L"capture" || mode == L"coverage" ||
                mode == L"native-motion")
            {
                ComPtr<IDxcCompiler> compiler;
                check(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
                ComPtr<IDxcBlobEncoding> disassembly;
                check(compiler->Disassemble(input.Get(), &disassembly));
                const auto assembly =
                    std::string_view((const char*) disassembly->GetBufferPointer(), disassembly->GetBufferSize());
                unsigned historyRegister = UINT32_MAX;
                if (argc == 7 && (mode == L"material" || mode == L"capture" || mode == L"coverage"))
                {
                    ComPtr<IDxcBlobEncoding> vertexBytes, vertexText;
                    check(library->CreateBlobFromFile(argv[6], &codepage, &vertexBytes));
                    check(compiler->Disassemble(vertexBytes.Get(), &vertexText));
                    auto vertex = GlassFg::RewriteVertexHistory(
                        std::string_view(static_cast<const char*>(vertexText->GetBufferPointer()),
                                         vertexText->GetBufferSize()),
                        layout);
                    if (!vertex)
                        throw std::runtime_error(vertex.error);
                    historyRegister = vertex.previousRegister;
                }
                auto patched =
                    mode == L"native-motion" ? GlassFg::ExtractNativeMotionTarget(assembly, unsigned(std::stoul(argv[5]))) : mode == L"rewrite"
                        ? GlassFg::RewriteVertexHistory(assembly, layout, cameraCapture ? &camera : nullptr,
                                                       pairCapture ? &clipPair : nullptr, inputCapture ? &inputPair : nullptr)
                        : GlassFg::RewriteMaterialMotion(
                              assembly, GlassFg::MaterialSource::One,
                              std::wstring(argv[5]) == L"dual" ? GlassFg::MaterialDestination::SecondSourceRgb
                                                               : GlassFg::MaterialDestination::OneMinusAlpha,
                              mode == L"capture" ? GlassFg::MaterialMotionTarget::OriginalColorAndCapture
                              : mode == L"coverage" ? GlassFg::MaterialMotionTarget::OriginalColorAndCoverage
                                                   : GlassFg::MaterialMotionTarget::SeparateTarget,
                              historyRegister, layout);
                if (!patched)
                    throw std::runtime_error(patched.error);
                check(library->CreateBlobWithEncodingOnHeapCopy(patched.assembly.data(),
                                                                (UINT32) patched.assembly.size(), CP_UTF8, &input));
            }
            ComPtr<IDxcAssembler> assembler;
            check(create(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler)));
            ComPtr<IDxcOperationResult> operation;
            check(assembler->AssembleToContainer(input.Get(), &operation));
            output = result(operation.Get());
            ComPtr<IDxcValidator> validator;
            check(create(CLSID_DxcValidator, IID_PPV_ARGS(&validator)));
            operation.Reset();
            check(validator->Validate(output.Get(), DxcValidatorFlags_InPlaceEdit, &operation));
            result(operation.Get());
        }
        else if (mode == L"compile")
        {
            ComPtr<IDxcCompiler> compiler;
            check(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
            ComPtr<IDxcOperationResult> operation;
            LPCWSTR args[] = { L"-O3", L"-Ges" };
            check(compiler->Compile(input.Get(), argv[3], L"main", argv[5], args, 2, nullptr, 0, nullptr, &operation));
            output = result(operation.Get());
        }
        else
            throw std::runtime_error("Unknown operation");
        std::ofstream file(argv[4], std::ios::binary);
        file.write((const char*) output->GetBufferPointer(), output->GetBufferSize());
        if (!file)
            throw std::runtime_error("Cannot write output");
        printf("OK bytes=%zu\n", output->GetBufferSize());
        return 0;
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
