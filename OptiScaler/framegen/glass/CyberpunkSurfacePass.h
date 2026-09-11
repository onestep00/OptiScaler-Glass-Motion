#pragma once
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "CyberpunkLayout.h"

namespace GlassFg
{
// Both instruction patterns must be unique and present in the transition's
// ordered call stack. Only relative call destinations may relocate; target code
// sections, remaining bytes and the resource contract must still agree.
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

  public:
    bool resolve(const RelocatableCode& image, const unsigned char* base)
    {
        using Target = RelocatableCode::Target;
        constexpr RelocatableCode::Reference rayRefs[] {
            { 17, 21, Target::Code }, { 28, 32, Target::Code }, { 40, 44, Target::Code }, { 53, 57, Target::Code }
        };
        constexpr RelocatableCode::Reference surfaceRefs[] { { 28, 32, Target::Code },
                                                             { 38, 42, Target::Code },
                                                             { 58, 62, Target::Code } };
        const auto ray = image.uniqueWindow(raySignature, rayRefs);
        const auto surface = image.uniqueWindow(surfaceSignature, surfaceRefs);
        rayDispatchCaller = ray && base ? base + ray + 32 : nullptr;
        surfaceConsumerCaller = surface && base ? base + surface + 32 : nullptr;
        return ready();
    }
    bool initialize(HMODULE executable, FILE* log)
    {
        rayDispatchCaller = surfaceConsumerCaller = nullptr;
        MODULEINFO info {};
        if (!IsCyberpunkExecutable(executable) ||
            !K32GetModuleInformation(GetCurrentProcess(), executable, &info, sizeof(info)) ||
            info.lpBaseOfDll != executable)
            return false;
        const auto* base = reinterpret_cast<const unsigned char*>(executable);
        RelocatableCode image;
        if (!image.initialize({ base, info.SizeOfImage }))
            return false;
        resolve(image, base);
        if (log)
            fprintf(log,
                    "SURFACE_PASS ready=%u ray_caller=%p surface_caller=%p relocatable_calls=1 resource_addresses=0\n",
                    ready(), rayDispatchCaller, surfaceConsumerCaller);
        return ready();
    }

    bool ready() const { return rayDispatchCaller && surfaceConsumerCaller; }
    std::array<const void*, 2> callers() const { return { rayDispatchCaller, surfaceConsumerCaller }; }

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
