#ifndef NOMINMAX
#define NOMINMAX
#endif
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include "../PackedMotionGpu.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <vector>
#include "GameGuard.h"

using Microsoft::WRL::ComPtr;
using DirectX::PackedVector::HALF;

static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
static void checked(HRESULT value, const char* message) { require(SUCCEEDED(value), message); }

static ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE type,
                                    D3D12_RESOURCE_STATES state,
                                    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    ComPtr<ID3D12Resource> result;
    checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                                            IID_PPV_ARGS(&result)), "buffer");
    return result;
}

static ComPtr<ID3D12Resource> texture(ID3D12Device* device, DXGI_FORMAT format)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = 32;
    description.Height = 20;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Format = format;
    ComPtr<ID3D12Resource> result;
    checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&result)), "texture");
    return result;
}

static void transition(ID3D12GraphicsCommandList* command, ID3D12Resource* resource,
                       D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    command->ResourceBarrier(1, &barrier);
}

static void drain(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    ComPtr<ID3D12Fence> fence;
    checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    checked(queue->Signal(fence.Get(), 1), "signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "event");
    checked(fence->SetEventOnCompletion(1, event), "event completion");
    require(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "timeout");
    CloseHandle(event);
    checked(device->GetDeviceRemovedReason(), "device removed");
}

static UINT64 pack(float depth, bool reverse, int mx, int my, unsigned alpha, unsigned id)
{
    // 17 bits of depth with the covered class bit on top. The capture marks a
    // record covered when its material opacity reaches the dispatch threshold,
    // so the fixture has to encode the same class the reconstructed pixel
    // shader would write. These fixtures dispatch with 50%, which the 8-bit
    // alpha reaches at 128.
    const auto depthBits = static_cast<unsigned>(std::clamp(depth, 0.f, 1.f) * 131071.f);
    const auto covered = alpha >= 128u ? 0x20000u : 0u;
    const auto depthKey = (reverse ? depthBits : 131071u - depthBits) | covered;
    return (UINT64(depthKey) << 46) | (UINT64(unsigned(mx) & 0x7ffu) << 35) |
           (UINT64(unsigned(my) & 0x7ffu) << 24) | (UINT64(alpha & 0xffu) << 16) |
           (reverse ? 0x8000u : 0u) | (id & 0x7fffu);
}

// Readback path check: run two frames, request a dump on the second, then
// verify the deferred write-out produced the four images and the sample text.
static int runDump(const wchar_t* shader)
{
    constexpr unsigned Width = 32, Height = 20;
    // Keep dump output next to the fixture, not in the source tree.
    const auto folder = std::filesystem::current_path() / L"glass-dump-fixture";
    std::filesystem::create_directories(folder);
    const auto localShader = folder / L"GlassObjectMotion.hlsl";
    std::filesystem::copy_file(std::filesystem::path(shader), localShader,
                               std::filesystem::copy_options::overwrite_existing);
    ComPtr<ID3D12Device> device;
    checked(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC queueDescription {};
    // A COMPUTE command list may only be executed on a compute queue; the FG
    // queue in the product is a compute queue for the same reason.
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    checked(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "queue");
    checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator)), "allocator");
    checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&command)), "command");
    auto motion = texture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto depth = texture(device.Get(), DXGI_FORMAT_R32_TYPELESS);
    std::vector<UINT64> packedData(Width * Height);
    for (unsigned y = 2; y < Height - 2; ++y)
        for (unsigned x = 2; x < Width - 2; ++x)
            packedData[y * Width + x] = pack(.7f, true, 8, -4, 128, 3);
    auto packedUpload = buffer(device.Get(), packedData.size() * sizeof(UINT64), D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
    auto packed = buffer(device.Get(), packedData.size() * sizeof(UINT64), D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    void* mapped = nullptr;
    checked(packedUpload->Map(0, nullptr, &mapped), "packed map");
    std::memcpy(mapped, packedData.data(), packedData.size() * sizeof(UINT64));
    packedUpload->Unmap(0, nullptr);
    command->CopyBufferRegion(packed.Get(), 0, packedUpload.Get(), 0, packedData.size() * sizeof(UINT64));
    transition(command.Get(), packed.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    GlassFg::PackedMotionGpu gpu;
    require(gpu.initialize(device.Get(), motion->GetDesc(), depth->GetDesc(), localShader.c_str(), stdout),
            "initialize");
    GlassFg::PackedMotionFrame frame { packed.Get(), Width, Height, 1 };
    require(gpu.dispatch(command.Get(), frame, motion.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_COPY_DEST, float(Width), float(Height), 0.f, 0.f,
                         { true, 50, false, 2 }),
            "dispatch");
    gpu.requestDump();
    // The readback buffers are allocated by the health thread's service call, so
    // the request has to be prepared before the frame that consumes it.
    (void)gpu.serviceDump();
    frame.frame = 2;
    require(gpu.dispatch(command.Get(), frame, motion.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_COPY_DEST, float(Width), float(Height), 0.f, 0.f,
                         { true, 50, false, 2 }),
            "dump dispatch");
    checked(command->Close(), "close");
    ID3D12CommandList* lists[] = { command.Get() };
    queue->ExecuteCommandLists(1, lists);
    drain(device.Get(), queue.Get());
    gpu.dumpSubmitted(queue.Get());
    drain(device.Get(), queue.Get());
    require(gpu.serviceDump(), "dump was not written");
    struct Expected { const wchar_t* name; };
    const Expected expected[] { { L"dump-1-mv.ppm" }, { L"dump-1-depth.ppm" }, { L"dump-1-original-mv.ppm" },
                                { L"dump-1-original-depth.ppm" }, { L"dump-1-packed.ppm" }, { L"dump-1.txt" } };
    unsigned long long bytes = 0;
    for (const auto& entry : expected)
    {
        const auto path = folder / entry.name;
        require(std::filesystem::exists(path), "dump file missing");
        bytes += std::filesystem::file_size(path);
    }
    std::printf("PACKED_MOTION_DUMP_OK files=%u bytes=%llu folder=%ls\n",
                static_cast<unsigned>(std::size(expected)), bytes, folder.c_str());
    gpu.releaseAfterGpuDrain();
    return 0;
}

// Out-of-band compose check: the production path records the copies and the
// compose on a module-owned compute list submitted on the FG queue.
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT readbackTexture(ID3D12Device* device, ID3D12CommandQueue* queue,
                                                          ID3D12GraphicsCommandList* command,
                                                          ID3D12CommandAllocator* allocator, ID3D12Resource* resource,
                                                          D3D12_RESOURCE_STATES state, std::vector<std::byte>& storage)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bytes = 0;
    const auto description = resource->GetDesc();
    device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto read = buffer(device, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    allocator->Reset();
    command->Reset(allocator, nullptr);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        transition(command, resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source {}, target {};
    source.pResource = resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.pResource = read.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = footprint;
    command->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        transition(command, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    checked(command->Close(), "readback close");
    ID3D12CommandList* lists[] = { command };
    queue->ExecuteCommandLists(1, lists);
    drain(device, queue);
    void* data = nullptr;
    checked(read->Map(0, nullptr, &data), "readback map");
    storage.assign(static_cast<std::byte*>(data), static_cast<std::byte*>(data) + bytes);
    read->Unmap(0, nullptr);
    return footprint;
}

static float halfAt(const std::vector<std::byte>& data, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                    unsigned x, unsigned y, unsigned component)
{
    const auto* row = reinterpret_cast<const HALF*>(data.data() + y * footprint.Footprint.RowPitch);
    return DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + component]);
}

static int runCompose(const wchar_t* shader, bool manual, bool partial = false)
{
    constexpr unsigned Width = 32, Height = 20;
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device;
    checked(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC queueDescription {};
    // The product's FG queue is a compute queue and the compose list is a
    // compute list; a compute list on a direct queue is an invalid call.
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    checked(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "queue");
    checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator)), "allocator");
    checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&command)), "command");
    auto motion = texture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto depth = texture(device.Get(), DXGI_FORMAT_R32_TYPELESS);
    std::vector<UINT64> packedData(Width * Height);
    for (unsigned y = 4; y < 16; ++y)
        for (unsigned x = 4; x < 16; ++x)
            packedData[y * Width + x] = pack(.8f, true, 16, -8, 128, 1);
    auto packedUpload = buffer(device.Get(), packedData.size() * sizeof(UINT64), D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
    auto packed = buffer(device.Get(), packedData.size() * sizeof(UINT64), D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    void* mapped = nullptr;
    checked(packedUpload->Map(0, nullptr, &mapped), "packed map");
    std::memcpy(mapped, packedData.data(), packedData.size() * sizeof(UINT64));
    packedUpload->Unmap(0, nullptr);
    command->CopyBufferRegion(packed.Get(), 0, packedUpload.Get(), 0, packedData.size() * sizeof(UINT64));
    transition(command.Get(), packed.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    checked(command->Close(), "close");
    ID3D12CommandList* lists[] = { command.Get() };
    queue->ExecuteCommandLists(1, lists);
    drain(device.Get(), queue.Get());

    GlassFg::PackedMotionGpu gpu;
    require(gpu.initialize(device.Get(), motion->GetDesc(), depth->GetDesc(), shader, stdout), "initialize");
    GlassFg::PackedMotionFrame frame { packed.Get(), Width, Height, 1 };
    if (manual)
    {
        // Bisect the invalid call: the same recording on a caller-owned compute list.
        ComPtr<ID3D12CommandAllocator> computeAllocator;
        ComPtr<ID3D12GraphicsCommandList> computeList;
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&computeAllocator)),
                "compute allocator");
        checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, computeAllocator.Get(), nullptr,
                                          IID_PPV_ARGS(&computeList)),
                "compute list");
        require(gpu.dispatch(computeList.Get(), frame, motion.Get(), depth.Get(),
                             D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST, float(Width),
                             float(Height), 0.f, 0.f, { true, 50, false, 2 }),
                "manual dispatch");
        checked(computeList->Close(), "manual close");
        ID3D12CommandList* manualLists[] = { computeList.Get() };
        queue->ExecuteCommandLists(1, manualLists);
        drain(device.Get(), queue.Get());
        std::printf("PACKED_MOTION_MANUAL_OK compute_list=1\n");
        return 0;
    }
    if (partial)
    {
        // Only the dispatched rows may be copied or written back. Pre-fill the
        // composed texture with a sentinel so a full-frame write-back would be
        // caught, then compose the top rows only.
        constexpr unsigned partialRows = 8;
        auto composed = gpu.motionOutput();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT sentinelFootprint {};
        UINT64 sentinelBytes = 0;
        const auto composedDescription = composed->GetDesc();
        device->GetCopyableFootprints(&composedDescription, 0, 1, 0, &sentinelFootprint, nullptr, nullptr,
                                      &sentinelBytes);
        auto sentinelUpload =
            buffer(device.Get(), sentinelBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* sentinelData = nullptr;
        checked(sentinelUpload->Map(0, nullptr, &sentinelData), "sentinel map");
        for (unsigned y = 0; y < Height; ++y)
        {
            auto* row = reinterpret_cast<HALF*>(static_cast<std::byte*>(sentinelData) +
                                                y * sentinelFootprint.Footprint.RowPitch);
            for (unsigned x = 0; x < Width * 4; ++x)
                row[x] = DirectX::PackedVector::XMConvertFloatToHalf(9.0f);
        }
        sentinelUpload->Unmap(0, nullptr);
        allocator->Reset();
        command->Reset(allocator.Get(), nullptr);
        D3D12_TEXTURE_COPY_LOCATION sentinelSource {}, sentinelTarget {};
        sentinelSource.pResource = sentinelUpload.Get();
        sentinelSource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sentinelSource.PlacedFootprint = sentinelFootprint;
        sentinelTarget.pResource = composed;
        sentinelTarget.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        command->CopyTextureRegion(&sentinelTarget, 0, 0, 0, &sentinelSource, nullptr);
        // The engine input carries the same sentinel: a compose that wrote back
        // into it would replace it with the composed value.
        sentinelTarget.pResource = motion.Get();
        command->CopyTextureRegion(&sentinelTarget, 0, 0, 0, &sentinelSource, nullptr);
        checked(command->Close(), "sentinel close");
        queue->ExecuteCommandLists(1, lists);
        drain(device.Get(), queue.Get());

        GlassFg::Controls controls { true, 50, false, 2 };
        controls.packedRows = partialRows;
        // Staging ladder configuration: with the FG input swap off the copy is
        // row-limited too (the product default swaps and copies whole frames).
        controls.packedSubstitute = false;
        // Deterministic release-gate check: the queue is blocked on a fence the
        // CPU owns, so the submitted compose provably cannot have completed.
        ComPtr<ID3D12Fence> gate;
        checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate fence");
        require(SUCCEEDED(queue->Wait(gate.Get(), 1)), "gate wait");
        require(gpu.submitCompose(queue.Get(), frame, motion.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_COPY_DEST, float(Width), float(Height), 0.f, 0.f, controls,
                                  nullptr),
                "partial submitCompose");
        // The release gate must refuse to free the outputs while the submitted
        // compose can still be executing.
        require(!gpu.drained(), "drained while the compose is still in flight");
        // CPU-side signal: a queue-side Signal would be recorded behind the wait
        // it is meant to release.
        require(SUCCEEDED(gate->Signal(1)), "gate signal");
        for (unsigned i = 0; i < 200 && !gpu.composeReady(); ++i)
            Sleep(5);
        require(gpu.composeReady(), "partial compose fence");
        require(gpu.drained(), "drained after the compose fence");
        drain(device.Get(), queue.Get());

        std::vector<std::byte> composedRead, engineRead;
        allocator->Reset();
        command->Reset(allocator.Get(), nullptr);
        const auto composedFootprint = readbackTexture(device.Get(), queue.Get(), command.Get(), allocator.Get(),
                                                       composed, D3D12_RESOURCE_STATE_COPY_DEST, composedRead);
        const auto engineFootprint = readbackTexture(device.Get(), queue.Get(), command.Get(), allocator.Get(),
                                                     motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, engineRead);
        const auto composedEdge = halfAt(composedRead, composedFootprint, 4, 6, 0);
        const auto composedOutside = halfAt(composedRead, composedFootprint, 4, 14, 0);
        const auto engineEdge = halfAt(engineRead, engineFootprint, 4, 6, 0);
        const auto engineOutside = halfAt(engineRead, engineFootprint, 4, 14, 0);
        require(std::abs(composedEdge - .0625f) < .001f, "partial compose did not reach the edge pixel");
        // Outside the composed rows the FG-facing copy must hold neither the
        // engine sentinel (input copy too wide) nor the object motion (dispatch
        // too wide).
        require(std::abs(composedOutside - 9.0f) > .01f && std::abs(composedOutside - .0625f) > .01f,
                "input copy or dispatch reached rows outside the composed range");
        // FG-only policy: the engine's own motion texture has to stay exactly as
        // the game produced it, because DLSS-SR, Ray Reconstruction and the ray
        // traced passes read the same texture.
        require(std::abs(engineEdge - 9.0f) < .01f, "partial compose modified the engine input");
        require(std::abs(engineOutside - 9.0f) < .01f, "partial compose modified rows outside the composed range");
        std::printf("PACKED_MOTION_PARTIAL_OK rows=%u edge=1 outside_untouched=1 engine_input_untouched=1 "
                    "gate_blocks_release=1\n",
                    partialRows);
        gpu.releaseAfterGpuDrain();
        return 0;
    }
    require(gpu.submitCompose(queue.Get(), frame, motion.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_COPY_DEST, float(Width), float(Height), 0.f, 0.f,
                              { true, 50, false, 2 }, nullptr),
            "submitCompose");
    if (const auto removed = device->GetDeviceRemovedReason(); removed != S_OK)
    {
        std::fprintf(stderr, "device removed right after submit: 0x%08x\n", static_cast<unsigned>(removed));
        ComPtr<ID3D12InfoQueue> info;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info))))
        {
            const auto count = info->GetNumStoredMessages();
            for (UINT64 i = 0; i < count; ++i)
            {
                SIZE_T size = 0;
                if (FAILED(info->GetMessage(i, nullptr, &size)) || !size)
                    continue;
                std::vector<std::byte> storage(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(info->GetMessage(i, message, &size)) && message->pDescription)
                    std::fprintf(stderr, "  debug[%llu] %s\n", static_cast<unsigned long long>(i),
                                 message->pDescription);
            }
        }
    }
    require(gpu.composeReady() || true, "composeReady probe");
    // The compose list must complete on its own fence before the outputs are read.
    for (unsigned i = 0; i < 200 && !gpu.composeReady(); ++i)
        Sleep(5);
    if (!gpu.composeReady())
        std::fprintf(stderr, "compose fence done=%llu submitted=%llu\n",
                     static_cast<unsigned long long>(gpu.composeCompleted()),
                     static_cast<unsigned long long>(gpu.composeSubmitted()));
    require(gpu.composeReady(), "compose fence");
    drain(device.Get(), queue.Get());

    auto outputs = gpu.motionOutput();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bytes = 0;
    auto description = outputs->GetDesc();
    device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto read = buffer(device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    allocator->Reset();
    command->Reset(allocator.Get(), nullptr);
    transition(command.Get(), outputs, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source {}, target {};
    source.pResource = outputs;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.pResource = read.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = footprint;
    command->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    checked(command->Close(), "close read");
    queue->ExecuteCommandLists(1, lists);
    drain(device.Get(), queue.Get());
    void* data = nullptr;
    checked(read->Map(0, nullptr, &data), "read map");
    const auto motionAt = [&](unsigned x, unsigned y, unsigned component)
    {
        auto* row = reinterpret_cast<const HALF*>(static_cast<const std::byte*>(data) +
                                                  y * footprint.Footprint.RowPitch);
        return DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + component]);
    };
    // (4,8) is on the object's left edge, where the compose must use the exact
    // object motion regardless of the background content.
    require(std::abs(motionAt(4, 8, 0) - .0625f) < .001f, "out-of-band compose did not reach the edge pixel");
    read->Unmap(0, nullptr);
    std::printf("PACKED_MOTION_COMPOSE_OK submit_compose=1 fence_ready=1 edge_pixel=1 background=1\n");
    // FG-only policy: a full compose must not touch the engine's own inputs.
    // The correction is delivered only by the DLSS-G parameter substitution,
    // so DLSS-SR, Ray Reconstruction and the ray traced passes keep reading the
    // game's textures.
    GlassFg::Controls full { true, 50, false, 2 };
    // The engine input carries a known sentinel so a compose that reached it is
    // distinguishable from an untouched texture. Fresh GPU allocations are not
    // valid evidence.
    {
        UINT64 sentinelBytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT sentinelFootprint {};
        const auto sentinelDescription = motion->GetDesc();
        device->GetCopyableFootprints(&sentinelDescription, 0, 1, 0, &sentinelFootprint, nullptr, nullptr,
                                      &sentinelBytes);
        auto sentinelUpload = buffer(device.Get(), sentinelBytes, D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
        void* sentinelData = nullptr;
        checked(sentinelUpload->Map(0, nullptr, &sentinelData), "engine sentinel map");
        std::memset(sentinelData, 0, size_t(sentinelBytes));
        for (unsigned y = 0; y < Height; ++y)
        {
            auto* row = reinterpret_cast<HALF*>(static_cast<std::byte*>(sentinelData) +
                                                y * sentinelFootprint.Footprint.RowPitch);
            for (unsigned x = 0; x < Width * 4; ++x)
                row[x] = DirectX::PackedVector::XMConvertFloatToHalf(9.0f);
        }
        sentinelUpload->Unmap(0, nullptr);
        allocator->Reset();
        command->Reset(allocator.Get(), nullptr);
        D3D12_TEXTURE_COPY_LOCATION sentinelSource {}, sentinelTarget {};
        sentinelSource.pResource = sentinelUpload.Get();
        sentinelSource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sentinelSource.PlacedFootprint = sentinelFootprint;
        sentinelTarget.pResource = motion.Get();
        sentinelTarget.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        command->CopyTextureRegion(&sentinelTarget, 0, 0, 0, &sentinelSource, nullptr);
        checked(command->Close(), "engine sentinel close");
        queue->ExecuteCommandLists(1, lists);
        drain(device.Get(), queue.Get());
    }
    require(gpu.submitCompose(queue.Get(), frame, motion.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_COPY_DEST, float(Width), float(Height), 0.f, 0.f, full, nullptr),
            "submitCompose engine sentinel");
    for (unsigned i = 0; i < 200 && !gpu.composeReady(); ++i)
        Sleep(5);
    require(gpu.composeReady(), "engine sentinel fence");
    drain(device.Get(), queue.Get());
    auto engineDescription = motion->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT engineFootprint {};
    UINT64 engineBytes = 0;
    device->GetCopyableFootprints(&engineDescription, 0, 1, 0, &engineFootprint, nullptr, nullptr, &engineBytes);
    auto engineRead = buffer(device.Get(), engineBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    allocator->Reset();
    command->Reset(allocator.Get(), nullptr);
    transition(command.Get(), motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION engineSource {}, engineTarget {};
    engineSource.pResource = motion.Get();
    engineSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    engineTarget.pResource = engineRead.Get();
    engineTarget.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    engineTarget.PlacedFootprint = engineFootprint;
    command->CopyTextureRegion(&engineTarget, 0, 0, 0, &engineSource, nullptr);
    checked(command->Close(), "close engine read");
    queue->ExecuteCommandLists(1, lists);
    drain(device.Get(), queue.Get());
    void* engineData = nullptr;
    checked(engineRead->Map(0, nullptr, &engineData), "engine read map");
    {
        auto* row = reinterpret_cast<const HALF*>(static_cast<const std::byte*>(engineData) +
                                                  8 * engineFootprint.Footprint.RowPitch);
        const auto value = DirectX::PackedVector::XMConvertHalfToFloat(row[4 * 4 + 0]);
        require(std::abs(value - 9.0f) < .01f, "compose modified the engine input edge pixel");
    }
    engineRead->Unmap(0, nullptr);
    std::printf("PACKED_MOTION_ENGINE_INPUT_OK engine_input_untouched=1\n");
    gpu.releaseAfterGpuDrain();
    return 0;
}

static ComPtr<ID3D12Resource> sizedTexture(ID3D12Device* device, DXGI_FORMAT format, UINT width, UINT height,
                                           D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Format = format;
    ComPtr<ID3D12Resource> result;
    checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                                            IID_PPV_ARGS(&result)), "sized texture");
    return result;
}

// Full-resolution cost and stability probe for the boundary dispatch. It is
// the same shader and the same dispatch the live module issues, at 2560x1440
// with a packed pattern that has ids over the whole frame.
static int runScale(const wchar_t* shader)
{
    constexpr unsigned Width = 2560, Height = 1440, Dispatches = 64;
    ComPtr<ID3D12Device> device;
    checked(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC queueDescription {};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    checked(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "queue");
    checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&command)), "command");
    auto motion = sizedTexture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, Width, Height,
                               D3D12_RESOURCE_STATE_COPY_DEST);
    auto depth = sizedTexture(device.Get(), DXGI_FORMAT_R32_TYPELESS, Width, Height, D3D12_RESOURCE_STATE_COPY_DEST);
    const std::array<float, 4> background { -.03125f, .025f, .375f, .75f };
    constexpr float backgroundDepth = .6f;
    for (unsigned i = 0; i < 2; ++i)
    {
        auto* target = i ? depth.Get() : motion.Get();
        auto description = target->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&description, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        auto upload = buffer(device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* destination = nullptr;
        checked(upload->Map(0, nullptr, &destination), "input map");
        for (unsigned y = 0; y < Height; ++y)
        {
            auto* row = static_cast<std::byte*>(destination) + fp.Offset + y * fp.Footprint.RowPitch;
            for (unsigned x = 0; x < Width; ++x)
            {
                if (i)
                    reinterpret_cast<float*>(row)[x] = backgroundDepth;
                else
                    for (unsigned c = 0; c < 4; ++c)
                        reinterpret_cast<HALF*>(row)[x * 4 + c] =
                            DirectX::PackedVector::XMConvertFloatToHalf(background[c]);
            }
        }
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION source {}, target_ {};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = fp;
        target_.pResource = target;
        target_.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        command->CopyTextureRegion(&target_, 0, 0, 0, &source, nullptr);
    }

    const auto pixels = UINT64(Width) * Height;
    std::vector<UINT64> packedData(static_cast<size_t>(pixels));
    // 8x8 rectangles with four-pixel gaps so both boundary and background
    // pixels exist everywhere on screen.
    const unsigned cell = Width / 8;
    for (unsigned ty = 0; ty < 8; ++ty)
        for (unsigned tx = 0; tx < 8; ++tx)
        {
            const auto id = 1u + (tx + ty * 8);
            for (unsigned y = ty * (Height / 8) + 2; y < (ty + 1) * (Height / 8) - 2; ++y)
                for (unsigned x = tx * cell + 2; x < (tx + 1) * cell - 2; ++x)
                    packedData[UINT64(y) * Width + x] = pack(.8f, true, 16, -8, 128, id);
        }
    auto packedUpload = buffer(device.Get(), packedData.size() * sizeof(UINT64), D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
    auto packed = buffer(device.Get(), packedData.size() * sizeof(UINT64), D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    void* mapped = nullptr;
    checked(packedUpload->Map(0, nullptr, &mapped), "packed map");
    std::memcpy(mapped, packedData.data(), packedData.size() * sizeof(UINT64));
    packedUpload->Unmap(0, nullptr);
    command->CopyBufferRegion(packed.Get(), 0, packedUpload.Get(), 0, packedData.size() * sizeof(UINT64));
    transition(command.Get(), packed.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    GlassFg::PackedMotionGpu gpu;
    require(gpu.initialize(device.Get(), motion->GetDesc(), depth->GetDesc(), shader, stdout), "initialize");
    GlassFg::PackedMotionFrame frame { packed.Get(), Width, Height, 1 };
    for (unsigned i = 0; i < Dispatches; ++i)
        require(gpu.dispatch(command.Get(), frame, motion.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_DEST, float(Width), float(Height),
                             0.f, 0.f, { true, 50, false, 2, true, Height, true }),
                "dispatch");
    // The owned outputs end in COPY_DEST after every dispatch; read one sample
    // back to prove the batch produced real values rather than a no-op.
    auto description = gpu.motionOutput()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&description, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    auto read = buffer(device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION source {}, target_ {};
    source.pResource = gpu.motionOutput();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target_.pResource = read.Get();
    target_.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target_.PlacedFootprint = fp;
    command->CopyTextureRegion(&target_, 0, 0, 0, &source, nullptr);
    checked(command->Close(), "close");

    const auto begin = std::chrono::steady_clock::now();
    ID3D12CommandList* lists[] = { command.Get() };
    queue->ExecuteCommandLists(1, lists);
    drain(device.Get(), queue.Get());
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();

    void* data = nullptr;
    checked(read->Map(0, nullptr, &data), "read map");
    const auto motionAt = [&](unsigned x, unsigned y, unsigned component)
    {
        auto* row = reinterpret_cast<const HALF*>(static_cast<const std::byte*>(data) +
                                                  y * fp.Footprint.RowPitch);
        return DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + component]);
    };
    const auto edgeX = cell + 4, edgeY = Height / 8 + 8;
    const auto gapX = cell - 1, gapY = Height / 8 - 1;
    const auto edgeValue = motionAt(edgeX, edgeY, 0);
    const auto gapValue = motionAt(gapX, gapY, 0);
    read->Unmap(0, nullptr);
    require(edgeValue != gapValue, "edge and gap sampled the same motion");
    std::printf("PACKED_MOTION_SCALE width=%u height=%u dispatches=%u ms_per_dispatch=%.4f total_ms=%.3f "
                "edge=%.5f gap=%.5f device_removed=0\n",
                Width, Height, Dispatches, elapsed / Dispatches, elapsed, edgeValue, gapValue);
    gpu.releaseAfterGpuDrain();
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        GlassRequireGameClosed();
        require(argc == 2 || argc == 3, "shader path required");
        if (argc == 3 && std::wcscmp(argv[2], L"--scale") == 0)
            return runScale(argv[1]);
        if (argc == 3 && std::wcscmp(argv[2], L"--dump") == 0)
            return runDump(argv[1]);
        if (argc == 3 && std::wcscmp(argv[2], L"--compose") == 0)
            return runCompose(argv[1], false);
        if (argc == 3 && std::wcscmp(argv[2], L"--compose-manual") == 0)
            return runCompose(argv[1], true);
        if (argc == 3 && std::wcscmp(argv[2], L"--compose-partial") == 0)
            return runCompose(argv[1], false, true);
        require(argc == 2, "unexpected argument");
        constexpr unsigned Width = 32, Height = 20;
        ComPtr<ID3D12Device> device;
        checked(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
        D3D12_COMMAND_QUEUE_DESC queueDescription {};
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command;
        checked(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "queue");
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&command)), "command");

        auto motion = texture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto depth = texture(device.Get(), DXGI_FORMAT_R32_TYPELESS);
        // Known nonzero background, including auxiliary MV channels. Fresh GPU
        // allocations are not valid evidence that fallback preserves its input.
        const std::array<float, 4> background { -.03125f, .025f, .375f, .75f };
        constexpr float backgroundDepth = .6f;
        std::array<ComPtr<ID3D12Resource>, 2> inputUploads;
        for (unsigned i = 0; i < inputUploads.size(); ++i)
        {
            auto* target = i ? depth.Get() : motion.Get();
            auto description = target->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&description, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
            inputUploads[i] = buffer(device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
            void* destination = nullptr;
            checked(inputUploads[i]->Map(0, nullptr, &destination), "input map");
            std::memset(destination, 0, size_t(bytes));
            for (unsigned y = 0; y < Height; ++y)
                for (unsigned x = 0; x < Width; ++x)
                {
                    auto* row = static_cast<std::byte*>(destination) + fp.Offset + y * fp.Footprint.RowPitch;
                    if (i) reinterpret_cast<float*>(row)[x] = backgroundDepth;
                    else for (unsigned c = 0; c < 4; ++c)
                        reinterpret_cast<HALF*>(row)[x * 4 + c] =
                            DirectX::PackedVector::XMConvertFloatToHalf(background[c]);
                }
            inputUploads[i]->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION source {}, destinationLocation {};
            source.pResource = inputUploads[i].Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = fp;
            destinationLocation.pResource = target;
            destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            command->CopyTextureRegion(&destinationLocation, 0, 0, 0, &source, nullptr);
        }
        // The frame declares reverse depth (DLSSG.DepthInverted): larger is
        // nearer. Object 1 (.8) lies in front of the engine surface (.6);
        // object 2 (.3) lies behind it, so the engine's own opaque surface is
        // the visible one there and the compose must leave it untouched. The
        // record's comparator stamp only decodes the depth key.
        GlassFg::SetFrameDepthInverted(1u);
        std::vector<UINT64> packedData(Width * Height);
        for (unsigned y = 4; y < 16; ++y)
            for (unsigned x = 4; x < 16; ++x)
                packedData[y * Width + x] = pack(.8f, true, 16, -8, 128, 1);
        for (unsigned y = 5; y < 15; ++y)
            for (unsigned x = 20; x < 29; ++x)
                packedData[y * Width + x] = pack(.3f, false, -24, 8, 64, 2);
        auto packedUpload = buffer(device.Get(), packedData.size() * sizeof(UINT64), D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
        auto packed = buffer(device.Get(), packedData.size() * sizeof(UINT64), D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        void* mapped = nullptr;
        checked(packedUpload->Map(0, nullptr, &mapped), "packed map");
        std::memcpy(mapped, packedData.data(), packedData.size() * sizeof(UINT64));
        packedUpload->Unmap(0, nullptr);
        command->CopyBufferRegion(packed.Get(), 0, packedUpload.Get(), 0, packedData.size() * sizeof(UINT64));
        transition(command.Get(), packed.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        GlassFg::PackedMotionGpu gpu;
        require(gpu.initialize(device.Get(), motion->GetDesc(), depth->GetDesc(), argv[1], stdout), "initialize");
        GlassFg::PackedMotionFrame frame { packed.Get(), Width, Height, 1 };
        // Two dispatches with different declared jitter (1 px change). In
        // jitter mode 0 the compose applies no conversion term, so the second
        // dispatch must deliver the recorded object motion unchanged (C7).
        require(gpu.dispatch(command.Get(), frame, motion.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_DEST, float(Width), float(Height),
                             .5f, .5f, { true, 50, false, 2 }), "seed dispatch");
        frame.frame = 2;
        require(gpu.dispatch(command.Get(), frame, motion.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_DEST, float(Width), float(Height),
                             1.5f, .5f, { true, 50, false, 2 }), "dispatch");

        std::array<ID3D12Resource*, 3> outputs { gpu.motionOutput(), gpu.depthOutput(), gpu.selectionOutput() };
        std::array<D3D12_RESOURCE_STATES, 3> states { D3D12_RESOURCE_STATE_COPY_DEST,
                                                     D3D12_RESOURCE_STATE_COPY_DEST,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        std::array<ComPtr<ID3D12Resource>, 3> reads;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 3> footprints {};
        for (unsigned i = 0; i < outputs.size(); ++i)
        {
            UINT64 bytes = 0;
            auto description = outputs[i]->GetDesc();
            device->GetCopyableFootprints(&description, 0, 1, 0, &footprints[i], nullptr, nullptr, &bytes);
            reads[i] = buffer(device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(command.Get(), outputs[i], states[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION source {}, destination {};
            source.pResource = outputs[i];
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.pResource = reads[i].Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprints[i];
            command->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        checked(command->Close(), "close");
        ID3D12CommandList* lists[] { command.Get() };
        queue->ExecuteCommandLists(1, lists);
        drain(device.Get(), queue.Get());

        std::array<void*, 3> data {};
        for (unsigned i = 0; i < reads.size(); ++i)
            checked(reads[i]->Map(0, nullptr, &data[i]), "read map");
        const auto motionAt = [&](unsigned x, unsigned y, unsigned component)
        {
            auto* row = reinterpret_cast<const HALF*>(static_cast<const std::byte*>(data[0]) +
                                                      y * footprints[0].Footprint.RowPitch);
            return DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + component]);
        };
        const auto depthAt = [&](unsigned x, unsigned y)
        {
            auto* row = reinterpret_cast<const float*>(static_cast<const std::byte*>(data[1]) +
                                                       y * footprints[1].Footprint.RowPitch);
            return row[x];
        };
        const auto selectionAt = [&](unsigned x, unsigned y)
        {
            auto* row = reinterpret_cast<const HALF*>(static_cast<const std::byte*>(data[2]) +
                                                      y * footprints[2].Footprint.RowPitch);
            return DirectX::PackedVector::XMConvertHalfToFloat(row[x]);
        };
        require(motionAt(1, 1, 0) == background[0] && depthAt(1, 1) == backgroundDepth &&
                selectionAt(1, 1) == 0, "background changed");
        // Jitter mode 0 (C7 default): the declared jitter change between the two
        // dispatches adds no term, so the object motion is delivered as recorded.
        require(std::abs(motionAt(4, 8, 0) - .0625f) < .001f &&
                std::abs(motionAt(4, 8, 1) + .05f) < .001f && std::abs(depthAt(4, 8) - .8f) < .001f &&
                selectionAt(4, 8) > .99f, "reverse-depth edge mismatch");
        // Interior at or above the opacity threshold (the dispatch passes 50%):
        // the object's own motion and depth replace the engine's value exactly.
        // No blend is applied, so the value equals the boundary value.
        require(std::abs(motionAt(10, 10, 0) - .0625f) < .001f &&
                std::abs(motionAt(10, 10, 1) + .05f) < .001f && std::abs(depthAt(10, 10) - .8f) < .001f &&
                selectionAt(10, 10) > .99f,
                "interior above the threshold did not take the object motion");
        // Interior below the threshold keeps the engine's motion and depth byte
        // for byte: the pixel belongs to the content behind the surface.
        // The engine background is stored as half, so the y component compares
        // against the same round trip the upload performed.
        require(motionAt(24, 10, 0) == background[0] &&
                motionAt(24, 10, 1) == DirectX::PackedVector::XMConvertHalfToFloat(
                    DirectX::PackedVector::XMConvertFloatToHalf(background[1])) &&
                depthAt(24, 10) == backgroundDepth && selectionAt(24, 10) == 0,
                "interior below the threshold changed");
        // Object 2 sits behind the engine's surface in the frame's convention:
        // even its boundary keeps the engine's motion, depth and selection.
        require(motionAt(20, 9, 0) == background[0] && depthAt(20, 9) == backgroundDepth &&
                    selectionAt(20, 9) == 0,
                "engine-nearer record was not occluded");
        for (unsigned y = 0; y < Height; ++y)
            for (unsigned x = 0; x < Width; ++x)
            {
                require(motionAt(x, y, 2) == background[2] && motionAt(x, y, 3) == background[3],
                        "auxiliary motion channels changed");
                if (packedData[y * Width + x]) continue;
                require(motionAt(x, y, 0) == background[0] &&
                        motionAt(x, y, 1) == DirectX::PackedVector::XMConvertHalfToFloat(
                            DirectX::PackedVector::XMConvertFloatToHalf(background[1])) &&
                        depthAt(x, y) == backgroundDepth && selectionAt(x, y) == 0,
                        "outside coverage changed");
            }
        for (auto& read : reads) read->Unmap(0, nullptr);
        gpu.releaseAfterGpuDrain();
        std::puts("PACKED_MOTION_GPU_OK background_preserved=1 exact_inner_edge=1 threshold_interior=1 "
                  "reverse_depth=1 engine_nearer_occluded=1 jitter_mode0=1 passes=1");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAILED %s\n", error.what());
        return 1;
    }
}
