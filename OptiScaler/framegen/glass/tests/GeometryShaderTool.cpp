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
        if (argc != 6)
            throw std::runtime_error("tool dxcompiler.dll disassemble|assemble|compile input output target");
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
        const std::wstring mode(argv[2]);
        if (mode == L"disassemble")
        {
            ComPtr<IDxcCompiler> compiler;
            check(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
            ComPtr<IDxcBlobEncoding> disassembly;
            check(compiler->Disassemble(input.Get(), &disassembly));
            output = disassembly;
        }
        else if (mode == L"assemble" || mode == L"rewrite" || mode == L"material" || mode == L"capture")
        {
            if (mode == L"rewrite" || mode == L"material" || mode == L"capture")
            {
                ComPtr<IDxcCompiler> compiler;
                check(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
                ComPtr<IDxcBlobEncoding> disassembly;
                check(compiler->Disassemble(input.Get(), &disassembly));
                const auto assembly =
                    std::string_view((const char*) disassembly->GetBufferPointer(), disassembly->GetBufferSize());
                auto patched =
                    mode == L"rewrite"
                        ? GlassFg::RewriteVertexHistory(assembly)
                        : GlassFg::RewriteMaterialMotion(
                              assembly, GlassFg::MaterialSource::One,
                              std::wstring(argv[5]) == L"dual" ? GlassFg::MaterialDestination::SecondSourceRgb
                                                               : GlassFg::MaterialDestination::OneMinusAlpha,
                              mode == L"capture" ? GlassFg::MaterialMotionTarget::OriginalColorAndCapture
                                                 : GlassFg::MaterialMotionTarget::SeparateTarget);
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
