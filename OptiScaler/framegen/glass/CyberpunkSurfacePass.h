#pragma once
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace GlassFg
{
// Version-specific identification of the observed surface-depth consumer pass.
// Both code fingerprints must be unique in the loaded executable and present
// in the transition's call stack. No heap address or pipeline pointer is saved.
// A different executable layout or modified fingerprint leaves FG unmodified.
class CyberpunkSurfacePass
{
    const void* rayDispatchCaller = nullptr;
    const void* surfaceConsumerCaller = nullptr;

    static constexpr unsigned char raySignature[64] = {
        0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1, 0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0xe1,
        0xe8, 0x4d, 0x39, 0x7b, 0xfd, 0x48, 0x8b, 0xc8, 0x4c, 0x8b, 0xe8, 0xe8, 0x4a, 0x6a, 0x7b, 0xfd,
        0x41, 0x8b, 0x14, 0x24, 0x49, 0x8b, 0xcd, 0xe8, 0xae, 0x76, 0x7b, 0xfd, 0x41, 0x8b, 0x54, 0x24,
        0x04, 0x49, 0x8b, 0xcd, 0xe8, 0xa1, 0x76, 0x7b, 0xfd, 0x41, 0x83, 0x7d, 0x68, 0x01, 0x75, 0x08
    };
    static constexpr unsigned char surfaceSignature[64] = {
        0x44, 0x24, 0x38, 0xa1, 0x86, 0x01, 0x00, 0x89, 0x7c, 0x24, 0x28, 0x44, 0x8b, 0x40, 0x38, 0x41,
        0x8d, 0x49, 0x05, 0x8b, 0x50, 0x34, 0x44, 0x89, 0x6c, 0x24, 0x20, 0xe8, 0x56, 0x83, 0xcc, 0x01,
        0x33, 0xd2, 0x49, 0x8b, 0xcf, 0xe8, 0x00, 0x0a, 0x58, 0xff, 0x48, 0x8b, 0x75, 0xb0, 0x49, 0x8b,
        0x4f, 0x08, 0x48, 0x8d, 0x55, 0xd0, 0x4d, 0x8b, 0xc4, 0xe8, 0x90, 0x50, 0x58, 0xff, 0x49, 0x8b
    };

    static const void* findUnique(const unsigned char* base, const IMAGE_NT_HEADERS64* nt,
                                  const unsigned char (&signature)[64])
    {
        const void* found = nullptr;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            const auto& section = sections[i];
            if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;
            const uint64_t endOffset = uint64_t(section.VirtualAddress) + section.Misc.VirtualSize;
            if (endOffset > nt->OptionalHeader.SizeOfImage || section.Misc.VirtualSize < sizeof(signature))
                return nullptr;
            auto begin = base + section.VirtualAddress;
            const auto end = base + endOffset;
            while (begin != end)
            {
                const auto at = std::search(begin, end, signature, signature + sizeof(signature));
                if (at == end)
                    break;
                if (found)
                    return nullptr;
                found = at + 32; // Captured return address within this fingerprint.
                begin = at + 1;
            }
        }
        return found;
    }

  public:
    bool initialize(HMODULE executable, FILE* log)
    {
        rayDispatchCaller = surfaceConsumerCaller = nullptr;
        if (!executable)
            return false;
        wchar_t path[32768] {};
        if (!GetModuleFileNameW(executable, path, 32768))
            return false;
        const auto* filename = wcsrchr(path, L'\\');
        if (_wcsicmp(filename ? filename + 1 : path, L"Cyberpunk2077.exe") != 0)
            return false;
        const auto* base = reinterpret_cast<const unsigned char*>(executable);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        // These identify the currently inspected build, not a general version promise.
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt->FileHeader.TimeDateStamp != 0x68af45ea ||
            nt->OptionalHeader.SizeOfImage != 0x4efc000 || nt->FileHeader.NumberOfSections == 0 ||
            nt->FileHeader.NumberOfSections > 96)
            return false;
        rayDispatchCaller = findUnique(base, nt, raySignature);
        surfaceConsumerCaller = findUnique(base, nt, surfaceSignature);
        if (log)
            fprintf(log, "SURFACE_PASS ready=%u ray_caller=%p surface_caller=%p resource_addresses=0\n", ready(),
                    rayDispatchCaller, surfaceConsumerCaller);
        return ready();
    }

    bool ready() const { return rayDispatchCaller && surfaceConsumerCaller; }

    bool matchesStack(void* const* stack, unsigned count) const
    {
        if (!ready() || !stack || count > 16)
            return false;
        bool raySeen = false;
        for (unsigned i = 0; i < count; ++i)
        {
            if (stack[i] == rayDispatchCaller)
                raySeen = true;
            if (raySeen && stack[i] == surfaceConsumerCaller)
                return true;
        }
        return false;
    }

    bool matches(const D3D12_RESOURCE_BARRIER& barrier, unsigned width, unsigned height) const
    {
        if (!ready() || !width || !height || barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
            barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE)
            return false;
        const auto& transition = barrier.Transition;
        constexpr auto expectedRead = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (!transition.pResource || transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
            transition.StateBefore != D3D12_RESOURCE_STATE_DEPTH_WRITE || transition.StateAfter != expectedRead)
            return false;
        const auto desc = transition.pResource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != width || desc.Height != height ||
            desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
            desc.Format != DXGI_FORMAT_R32_TYPELESS || !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
            return false;
        void* stack[16] {};
        return matchesStack(stack, CaptureStackBackTrace(0, 16, stack, nullptr));
    }
};
} // namespace GlassFg
