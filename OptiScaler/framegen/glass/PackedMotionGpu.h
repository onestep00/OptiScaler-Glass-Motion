#pragma once
#include "GlassControls.h"
#include "GlassGpuTimer.h"
#include "PackedMotionCapture.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace GlassFg
{
class PackedMotionGpu
{
    ID3D12Device* device = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* selection = nullptr;
    // FG-facing copies of motion/depth. They are written only by copy, so the
    // evaluation never consumes a UAV-written resource (see dispatch()).
    ID3D12Resource* motionRead = nullptr;
    ID3D12Resource* depthRead = nullptr;
    // Diagnostic coverage counters written by the compose shader:
    // [dispatched, packed_id, edge, interior].
    ID3D12Resource* counters = nullptr;
    ID3D12Resource* zeroCounters = nullptr;
    unsigned width = 0, height = 0, increment = 0;
    // Diagnostic readback. Nothing is allocated or copied until the live
    // channel asks for a dump, so the correction path pays nothing by default.
    // 0 composed motion, 1 composed depth, 2 coverage counters,
    // 3 original motion, 4 original depth (same-frame comparison),
    // 5 packed object records (engine coverage).
    ID3D12Resource* readback[6] {};
    UINT64 readbackBytes[6] {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readbackFootprint[6] {};
    unsigned packedCovered = 0;
    ID3D12Fence* dumpFence = nullptr;
    UINT64 dumpValue = 0;
    unsigned dumpSerial = 0, dumpFrame = 0;
    bool dumpPending = false;
    std::atomic<unsigned> dumpRequests { 0 };
    std::filesystem::path dumpFolder;
    FILE* logFile = nullptr;
    // Out-of-band compose. Recorded on our own compute list and submitted on
    // the FG queue before the batch that carries the FG call, so the engine's
    // command list state (descriptor heaps, root signature, PSO) is never
    // modified and the NGX recording sequence stays untouched.
    ID3D12CommandAllocator* composeAllocator = nullptr;
    ID3D12GraphicsCommandList* composeList = nullptr;
    ID3D12Fence* composeFence = nullptr;
    std::uint64_t composeValue = 0;
    bool composePending = false;
    // The compose list is submitted without the host completion signal whenever
    // the input swap is inactive, so the session teardown needs its own proof
    // that the list finished before these outputs are freed. Freeing them while
    // the GPU still executes the compose is the 2026-09-15 reset.
    std::chrono::steady_clock::time_point composeSubmittedAt {};
    std::uint64_t composeForcedReleases = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned index) const
    {
        auto value = heap->GetCPUDescriptorHandleForHeapStart();
        value.ptr += SIZE_T(index) * increment;
        return value;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned index) const
    {
        auto value = heap->GetGPUDescriptorHandleForHeapStart();
        value.ptr += UINT64(index) * increment;
        return value;
    }
    static void transition(ID3D12GraphicsCommandList* command, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        command->ResourceBarrier(1, &barrier);
    }
    bool createTexture(D3D12_RESOURCE_DESC desc, D3D12_RESOURCE_STATES state, ID3D12Resource** output)
    {
        D3D12_HEAP_PROPERTIES properties {};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        desc.Alignment = 0;
        return SUCCEEDED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                         IID_PPV_ARGS(output)));
    }
    // Compiles the compose shader and swaps the compute PSO. Safe to call again
    // from the live debug channel; the old PSO stays until the new one exists.
    bool compilePipeline(const wchar_t* shader, FILE* log)
    {
        FILE* file = _wfopen(shader, L"rb");
        if (!file)
            return false;
        std::fseek(file, 0, SEEK_END);
        const auto size = std::ftell(file);
        std::rewind(file);
        std::vector<char> source(size > 0 ? static_cast<size_t>(size) : 0);
        const bool read = size > 0 && std::fread(source.data(), 1, source.size(), file) == source.size();
        std::fclose(file);
        if (!read)
            return false;
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        auto result = D3DCompile(source.data(), source.size(), "glass-object-motion.hlsl", nullptr, nullptr,
                                 "ApplyObjectMotion", "cs_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        if (errors)
        {
            std::fprintf(log, "OBJECT_SHADER %s\n", static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        if (FAILED(result))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription {};
        pipelineDescription.pRootSignature = root;
        pipelineDescription.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        ID3D12PipelineState* created = nullptr;
        result = device->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&created));
        code->Release();
        if (FAILED(result))
            return false;
        if (pipeline)
            pipeline->Release();
        pipeline = created;
        return true;
    }

  public:
    PackedMotionGpu() = default;
    PackedMotionGpu(const PackedMotionGpu&) = delete;
    PackedMotionGpu& operator=(const PackedMotionGpu&) = delete;

    bool initialize(ID3D12Device* value, const D3D12_RESOURCE_DESC& motionDescription,
                    const D3D12_RESOURCE_DESC& depthDescription, const wchar_t* shader, FILE* log)
    {
        if (device || !value || !shader || !log || motionDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            depthDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !motionDescription.Width ||
            motionDescription.Width > 32768 || !motionDescription.Height || motionDescription.Height > 32768 ||
            motionDescription.Width != depthDescription.Width || motionDescription.Height != depthDescription.Height ||
            motionDescription.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            (depthDescription.Format != DXGI_FORMAT_R32_FLOAT && depthDescription.Format != DXGI_FORMAT_R32_TYPELESS))
            return false;
        device = value;
        this->logFile = log;
        dumpFolder = std::filesystem::path(shader).parent_path();
        width = static_cast<unsigned>(motionDescription.Width);
        height = motionDescription.Height;
        // Size/format evidence for crash attribution: a copy box larger than a
        // bound texture or a format mismatch is the first suspect when a driver
        // reset follows the first full-height frame.
        if (logFile)
        {
            std::fprintf(logFile,
                         "PACKED_GPU extent=%ux%u motion_format=%u depth_format=%u depth_flags=%u\n",
                         width, height, static_cast<unsigned>(motionDescription.Format),
                         static_cast<unsigned>(depthDescription.Format),
                         static_cast<unsigned>(depthDescription.Flags));
            std::fflush(logFile);
        }

        auto outputMotion = motionDescription;
        outputMotion.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!createTexture(outputMotion, D3D12_RESOURCE_STATE_COPY_DEST, &motion))
            return false;
        auto outputDepth = depthDescription;
        outputDepth.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!createTexture(outputDepth, D3D12_RESOURCE_STATE_COPY_DEST, &depth))
            return false;
        // The FG evaluation consumes these two. They are only ever written by a
        // copy, never by the compose UAV: handing UAV-written resources to the
        // evaluation reset the driver on 2026-09-14 23:17, while the copy-only
        // pair was stable in the 22:57 session.
        if (!createTexture(motionDescription, D3D12_RESOURCE_STATE_COPY_DEST, &motionRead) ||
            !createTexture(depthDescription, D3D12_RESOURCE_STATE_COPY_DEST, &depthRead))
            return false;
        auto selected = depthDescription;
        selected.Format = DXGI_FORMAT_R16_FLOAT;
        selected.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!createTexture(selected, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &selection))
            return false;

        D3D12_HEAP_PROPERTIES counterHeap {};
        counterHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC counterDescription {};
        counterDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        counterDescription.Width = 64;
        counterDescription.Height = 1;
        counterDescription.DepthOrArraySize = 1;
        counterDescription.MipLevels = 1;
        counterDescription.SampleDesc.Count = 1;
        counterDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        counterDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&counterHeap, D3D12_HEAP_FLAG_NONE, &counterDescription,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&counters))))
            return false;
        counterDescription.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES uploadCounterHeap {};
        uploadCounterHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device->CreateCommittedResource(&uploadCounterHeap, D3D12_HEAP_FLAG_NONE, &counterDescription,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&zeroCounters))))
            return false;
        void* zeros = nullptr;
        if (SUCCEEDED(zeroCounters->Map(0, nullptr, &zeros)) && zeros)
        {
            std::memset(zeros, 0, 64);
            zeroCounters->Unmap(0, nullptr);
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDescription {};
        heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDescription.NumDescriptors = 4;
        heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&heap))))
            return false;
        increment = device->GetDescriptorHandleIncrementSize(heapDescription.Type);
        ID3D12Resource* outputs[] { motion, depth, selection };
        DXGI_FORMAT formats[] { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT };
        for (unsigned i = 0; i < 3; ++i)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
            view.Format = formats[i];
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(outputs[i], nullptr, &view, cpu(i));
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC counterView {};
        counterView.Format = DXGI_FORMAT_R32_TYPELESS;
        counterView.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        counterView.Buffer.NumElements = 16;
        counterView.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(counters, nullptr, &counterView, cpu(3));

        D3D12_DESCRIPTOR_RANGE range {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 4;
        D3D12_ROOT_PARAMETER parameters[3] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[0].Descriptor = { 0, 0 };
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = { 1, &range };
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants = { 0, 0, 8 };
        D3D12_ROOT_SIGNATURE_DESC rootDescription { 3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob* serialized = nullptr;
        ID3DBlob* errors = nullptr;
        auto result = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
        if (errors)
        {
            std::fprintf(log, "OBJECT_ROOT %s\n", static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        if (FAILED(result))
            return false;
        result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&root));
        serialized->Release();
        if (FAILED(result))
            return false;

        if (!compilePipeline(shader, log))
            return false;
        std::fprintf(log, "OBJECT_MOTION_GPU_READY %ux%u passes=1 edge_samples_max=32\n", width, height);
        std::fflush(log);
        return true;
    }

    bool reload(const wchar_t* shader, FILE* log)
    {
        if (!device || !root || !shader || !log)
            return false;
        if (!compilePipeline(shader, log))
            return false;
        std::fprintf(log, "OBJECT_MOTION_GPU_RELOADED %ux%u\n", width, height);
        std::fflush(log);
        return true;
    }

    // Live channel: dump one frame's composed motion and depth as PPM images
    // plus a text sample grid. Diagnostic only: one copy of each texture and a
    // blocking map on the health thread, nothing on the normal path.
    void requestDump() noexcept { dumpRequests.fetch_add(1, std::memory_order_relaxed); }

    void dumpSubmitted(ID3D12CommandQueue* queue) noexcept
    {
        if (!dumpPending || !queue || !dumpFence)
            return;
        if (SUCCEEDED(queue->Signal(dumpFence, ++dumpValue)) && logFile)
        {
            std::fprintf(logFile, "PACKED_DUMP submitted serial=%u frame=%u value=%llu\n", dumpSerial, dumpFrame,
                         static_cast<unsigned long long>(dumpValue));
            std::fflush(logFile);
        }
    }

    bool serviceDump() noexcept
    {
        if (!dumpPending || !dumpFence || !readback[0] || !readback[1])
            return false;
        const auto completed = dumpFence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < dumpValue)
            return false;
        void* data[6] {};
        for (unsigned i = 0; i < 6; ++i)
            if (FAILED(readback[i]->Map(0, nullptr, &data[i])) || !data[i])
            {
                for (unsigned j = 0; j < i; ++j)
                    readback[j]->Unmap(0, nullptr);
                dumpPending = false;
                return false;
            }
        const auto folder = dumpFolder.empty() ? std::filesystem::path(L"Glass") : dumpFolder;
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        const auto suffix = std::to_wstring(dumpSerial);
        const auto base = (folder / L"dump").wstring() + L"-" + suffix;
        writeMotion(base + L"-mv.ppm", static_cast<const std::byte*>(data[0]));
        writeDepth(base + L"-depth.ppm", static_cast<const std::byte*>(data[1]));
        writeMotion(base + L"-original-mv.ppm", static_cast<const std::byte*>(data[3]));
        writeDepth(base + L"-original-depth.ppm", static_cast<const std::byte*>(data[4]));
        writePacked(base + L"-packed.ppm", static_cast<const std::byte*>(data[5]));
        writeSamples(base + L".txt", static_cast<const std::byte*>(data[0]), static_cast<const std::byte*>(data[1]),
                     static_cast<const std::byte*>(data[2]), static_cast<const std::byte*>(data[3]),
                     static_cast<const std::byte*>(data[4]));
        for (unsigned i = 0; i < 6; ++i)
            readback[i]->Unmap(0, nullptr);
        if (logFile)
        {
            std::fprintf(logFile, "PACKED_DUMP written serial=%u frame=%u path=%ls\n", dumpSerial, dumpFrame,
                         base.c_str());
            std::fflush(logFile);
        }
        dumpPending = false;
        return true;
    }

  public:
    bool composeReady()
    {
        if (!composePending)
            return true;
        if (!composeFence)
            return false;
        const auto completed = composeFence->GetCompletedValue();
        if (completed == UINT64_MAX)
            return false;
        if (completed >= composeValue)
            composePending = false;
        return !composePending;
    }
    // Diagnostics: raw fence state for the live channel and the offline fixture.
    std::uint64_t composeCompleted() const
    {
        return composeFence ? composeFence->GetCompletedValue() : 0;
    }
    std::uint64_t composeSubmitted() const { return composeValue; }
    bool composeInFlight() const { return composePending; }
    std::uint64_t composeForced() const { return composeForcedReleases; }
    // Teardown gate: true only when no compose list of this object can still be
    // executing on the GPU. A submission whose completion never arrives would
    // otherwise block admission forever, so past the driver reset window the
    // outputs are released with a counted, logged escape.
    bool drained()
    {
        if (!composePending)
            return true;
        if (composeFence)
        {
            const auto completed = composeFence->GetCompletedValue();
            if (completed != UINT64_MAX && completed >= composeValue)
            {
                composePending = false;
                return true;
            }
        }
        if (std::chrono::steady_clock::now() - composeSubmittedAt > std::chrono::seconds(5))
        {
            composePending = false;
            ++composeForcedReleases;
            return true;
        }
        return false;
    }

    // Records the input copies, the compose dispatch and the optional engine
    // write-back on our own compute list, then submits it on the FG queue. The
    // caller performs the producer wait on that queue before this call, so the
    // engine's command list state and the NGX recording sequence stay clean.
    bool submitCompose(ID3D12CommandQueue* queue, const PackedMotionFrame& packed, ID3D12Resource* originalMotion,
                       ID3D12Resource* originalDepth, D3D12_RESOURCE_STATES motionState,
                       D3D12_RESOURCE_STATES depthState, float scaleX, float scaleY, Controls controls,
                       GpuTimer* timer)
    {
        if (!queue || !composeReady() || !ensureCompose())
            return false;
        if (FAILED(composeAllocator->Reset()) || FAILED(composeList->Reset(composeAllocator, nullptr)))
            return false;
        // Sparse GPU timing on the host's existing timer. This is a compute list
        // on the FG queue, the same shape the timer already accepts; a full
        // eight-slot window skips the sample instead of waiting.
        const auto timing = timer && controls.measureGpuTime ? timer->begin(composeList) : GpuTimer::Ticket {};
        const auto rows = (std::min)(height, (std::max)(1u, controls.packedRows));
        const bool recorded =
            dispatch(composeList, packed, originalMotion, originalDepth, motionState, depthState, scaleX, scaleY,
                     controls) &&
            (!controls.packedWriteBack ||
             writeBack(composeList, originalMotion, originalDepth, motionState, depthState, rows));
        if (timing)
            timer->end(composeList, timing);
        if (!recorded || FAILED(composeList->Close()))
            return false;
        ID3D12CommandList* lists[] { composeList };
        queue->ExecuteCommandLists(1, lists);
        composeSubmittedAt = std::chrono::steady_clock::now();
        const auto value = ++composeValue;
        // Pending is set before the signal on purpose: a list that was already
        // submitted must never be reclaimed by a later Reset while its
        // completion is unknown.
        composePending = true;
        if (FAILED(queue->Signal(composeFence, value)))
            return false;
        if (timing)
            timer->submitted(composeList, queue, composeFence, composeValue);
        if (controls.trace && logFile)
        {
            std::fprintf(logFile, "TRACE_COMPOSE queue=%p frame=%u rows=%u writeback=%u\n", queue, packed.frame,
                         (std::min)(height, (std::max)(1u, controls.packedRows)), controls.packedWriteBack ? 1u : 0u);
            std::fflush(logFile);
        }
        return true;
    }

  private:
    bool ensureCompose()
    {
        if (composeList)
            return true;
        if (!device ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&composeAllocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, composeAllocator, nullptr,
                                             IID_PPV_ARGS(&composeList))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&composeFence))))
            return false;
        composeList->Close();
        return true;
    }

    bool prepareReadback()
    {
        if (readback[0] && readback[1] && readback[2] && readback[3] && readback[4] && readback[5])
            return true;
        ID3D12Resource* targets[] { motion, depth };
        for (unsigned i = 0; i < 2; ++i)
        {
            if (readback[i])
                continue;
            auto description = targets[i]->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            D3D12_HEAP_PROPERTIES properties {};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bufferDescription {};
            bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDescription.Width = bytes;
            bufferDescription.Height = 1;
            bufferDescription.DepthOrArraySize = 1;
            bufferDescription.MipLevels = 1;
            bufferDescription.SampleDesc.Count = 1;
            bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* created = nullptr;
            if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&created))))
                return false;
            readback[i] = created;
            readbackBytes[i] = bytes;
            readbackFootprint[i] = footprint;
        }
        if (!dumpFence && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dumpFence))))
            return false;
        if (!readback[2])
        {
            D3D12_HEAP_PROPERTIES properties {};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bufferDescription {};
            bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDescription.Width = 64;
            bufferDescription.Height = 1;
            bufferDescription.DepthOrArraySize = 1;
            bufferDescription.MipLevels = 1;
            bufferDescription.SampleDesc.Count = 1;
            bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* created = nullptr;
            if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&created))))
                return false;
            readback[2] = created;
        }
        // The original FG inputs share the owned textures' descriptions, so
        // the same footprint and size serve the comparison readbacks.
        for (unsigned i = 3; i < 5; ++i)
        {
            if (readback[i])
                continue;
            const auto source = i - 3;
            D3D12_HEAP_PROPERTIES properties {};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bufferDescription {};
            bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDescription.Width = readbackBytes[source];
            bufferDescription.Height = 1;
            bufferDescription.DepthOrArraySize = 1;
            bufferDescription.MipLevels = 1;
            bufferDescription.SampleDesc.Count = 1;
            bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* created = nullptr;
            if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&created))))
                return false;
            readback[i] = created;
            readbackFootprint[i] = readbackFootprint[source];
        }
        if (!readback[5])
        {
            D3D12_HEAP_PROPERTIES properties {};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bufferDescription {};
            bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDescription.Width = UINT64(width) * height * 8;
            bufferDescription.Height = 1;
            bufferDescription.DepthOrArraySize = 1;
            bufferDescription.MipLevels = 1;
            bufferDescription.SampleDesc.Count = 1;
            bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* created = nullptr;
            if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&created))))
                return false;
            readback[5] = created;
            readbackBytes[5] = bufferDescription.Width;
        }
        ++dumpSerial;
        return true;
    }

    const std::byte* readbackRow(const std::byte* data, unsigned index, unsigned y) const
    {
        return data + readbackFootprint[index].Offset + UINT64(y) * readbackFootprint[index].Footprint.RowPitch;
    }

    static std::byte toByte(float value)
    {
        return static_cast<std::byte>(std::clamp(value, 0.f, 255.f));
    }

    static float halfToFloat(unsigned short value)
    {
        const auto sign = unsigned(value & 0x8000u) << 16;
        auto exponent = unsigned((value >> 10) & 0x1fu);
        auto mantissa = unsigned(value & 0x3ffu);
        unsigned bits = 0;
        if (!exponent)
        {
            if (!mantissa)
                bits = sign;
            else
            {
                auto adjusted = 127 - 15 + 1;
                while (!(mantissa & 0x400u))
                {
                    mantissa <<= 1;
                    --adjusted;
                }
                bits = sign | (unsigned(adjusted) << 23) | ((mantissa & 0x3ffu) << 13);
            }
        }
        else if (exponent == 31)
            bits = sign | 0x7f800000u | (mantissa << 13);
        else
            bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
        float result = 0.f;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    void writeMotion(const std::wstring& path, const std::byte* data) const
    {
        FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file)
            return;
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        for (unsigned y = 0; y < height; ++y)
        {
            const auto* row = reinterpret_cast<const unsigned short*>(readbackRow(data, 0, y));
            for (unsigned x = 0; x < width; ++x)
            {
                const auto mx = halfToFloat(row[x * 4 + 0]);
                const auto my = halfToFloat(row[x * 4 + 1]);
                const std::byte pixel[3] { toByte(128.f + mx * 512.f), toByte(128.f + my * 512.f),
                                           toByte(128.f + std::sqrt(mx * mx + my * my) * 1024.f) };
                std::fwrite(pixel, 1, 3, file);
            }
        }
        std::fclose(file);
    }

    void writeDepth(const std::wstring& path, const std::byte* data) const
    {
        FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file)
            return;
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        for (unsigned y = 0; y < height; ++y)
        {
            const auto* row = reinterpret_cast<const float*>(readbackRow(data, 1, y));
            for (unsigned x = 0; x < width; ++x)
            {
                const std::byte pixel[3] { toByte(row[x] * 255.f), toByte(row[x] * 255.f), toByte(row[x] * 255.f) };
                std::fwrite(pixel, 1, 3, file);
            }
        }
        std::fclose(file);
    }

    // Engine coverage image: which pixels carry an object record, tinted by
    // object id and brightened by the depth key.
    void writePacked(const std::wstring& path, const std::byte* data)
    {
        FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file)
            return;
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        unsigned covered = 0;
        for (unsigned y = 0; y < height; ++y)
        {
            const auto* row = reinterpret_cast<const std::uint64_t*>(data + UINT64(y) * width * 8);
            for (unsigned x = 0; x < width; ++x)
            {
                const auto record = row[x];
                const auto id = unsigned(record & 0x7fffu);
                const auto depthKey = unsigned((record >> 46) & 0x3ffffu);
                std::byte pixel[3] { std::byte(0), std::byte(0), std::byte(0) };
                if (id)
                {
                    ++covered;
                    const auto hue = unsigned((id * 2654435761u) >> 24) & 0xffu;
                    pixel[0] = static_cast<std::byte>(64 + ((hue * 3) & 0xbf));
                    pixel[1] = static_cast<std::byte>(64 + ((hue * 5) & 0xbf));
                    pixel[2] = static_cast<std::byte>(64 + (depthKey * 255u / 262143u));
                }
                std::fwrite(pixel, 1, 3, file);
            }
        }
        packedCovered = covered;
        std::fclose(file);
    }

    void writeSamples(const std::wstring& path, const std::byte* motionData, const std::byte* depthData,
                      const std::byte* counterData, const std::byte* originalMotionData,
                      const std::byte* originalDepthData) const
    {
        FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file)
            return;
        std::fprintf(file, "serial=%u frame=%u size=%ux%u engine_covered_pixels=%u\n", dumpSerial, dumpFrame, width,
                     height, packedCovered);
        if (counterData)
        {
            const auto* value = reinterpret_cast<const unsigned*>(counterData);
            std::fprintf(file, "dispatched_pixels=%u packed_pixels=%u edge_pixels=%u interior_pixels=%u\n", value[0],
                         value[1], value[2], value[3]);
        }
        for (unsigned gy = 0; gy < 9; ++gy)
        {
            for (unsigned gx = 0; gx < 16; ++gx)
            {
                const auto x = (gx * width) / 16, y = (gy * height) / 9;
                const auto* row = reinterpret_cast<const unsigned short*>(readbackRow(motionData, 0, y));
                const auto* depthRow = reinterpret_cast<const float*>(readbackRow(depthData, 1, y));
                const auto* originalRow =
                    reinterpret_cast<const unsigned short*>(readbackRow(originalMotionData, 3, y));
                const auto* originalDepthRow =
                    reinterpret_cast<const float*>(readbackRow(originalDepthData, 4, y));
                std::fprintf(file,
                             "x=%u y=%u mv=(%.6f,%.6f) original_mv=(%.6f,%.6f) depth=%.6f original_depth=%.6f\n", x, y,
                             halfToFloat(row[x * 4 + 0]), halfToFloat(row[x * 4 + 1]),
                             halfToFloat(originalRow[x * 4 + 0]), halfToFloat(originalRow[x * 4 + 1]), depthRow[x],
                             originalDepthRow[x]);
            }
        }
        std::fclose(file);
    }

  public:
    // Integration without foreign resources: write the composed motion/depth
    // back into the game's own FG inputs. Both targets are already in
    // COPY_DEST, the state the FG boundary hands them over in.
    bool writeBack(ID3D12GraphicsCommandList* command, ID3D12Resource* targetMotion, ID3D12Resource* targetDepth,
                   D3D12_RESOURCE_STATES motionState, D3D12_RESOURCE_STATES depthState, unsigned rows)
    {
        if (!command || !targetMotion || !targetDepth || !motion || !depth)
            return false;
        transition(command, motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(command, depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (motionState != D3D12_RESOURCE_STATE_COPY_DEST)
            transition(command, targetMotion, motionState, D3D12_RESOURCE_STATE_COPY_DEST);
        if (depthState != D3D12_RESOURCE_STATE_COPY_DEST)
            transition(command, targetDepth, depthState, D3D12_RESOURCE_STATE_COPY_DEST);
        // Only the rows the shader composed can differ from the engine's own
        // inputs; copying the rest is pure bandwidth and would also clobber
        // them with this module's scratch texture.
        const unsigned copyRows = (std::min)(rows ? rows : height, height);
        const D3D12_BOX box { 0, 0, 0, width, copyRows, 1 };
        D3D12_TEXTURE_COPY_LOCATION source {}, target {};
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.pResource = motion;
        target.pResource = targetMotion;
        command->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
        source.pResource = depth;
        target.pResource = targetDepth;
        command->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
        if (motionState != D3D12_RESOURCE_STATE_COPY_DEST)
            transition(command, targetMotion, D3D12_RESOURCE_STATE_COPY_DEST, motionState);
        if (depthState != D3D12_RESOURCE_STATE_COPY_DEST)
            transition(command, targetDepth, D3D12_RESOURCE_STATE_COPY_DEST, depthState);
        transition(command, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(command, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        return true;
    }

    bool dispatch(ID3D12GraphicsCommandList* command, const PackedMotionFrame& packed,
                  ID3D12Resource* originalMotion, ID3D12Resource* originalDepth,
                  D3D12_RESOURCE_STATES motionState, D3D12_RESOURCE_STATES depthState,
                  float scaleX, float scaleY, Controls controls)
    {
        if (!pipeline || !command || !packed || packed.width != width || packed.height != height ||
            !originalMotion || !originalDepth || scaleX <= 0 || scaleY <= 0)
            return false;
        if (motionState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalMotion, motionState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalDepth, depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        // The staged row limit controls the new compute work only. Any output
        // that the FG evaluation consumes must hold a complete frame: rows the
        // compute never touches would otherwise keep stale memory, and NGX
        // reading that is wrong by construction (the 22:49 reset ran with 240
        // composed rows and a stale remainder). A pending dump reads the whole
        // frame as well.
        const unsigned copyRows = (dumpRequests.load(std::memory_order_relaxed) || controls.packedSubstitute)
                                      ? height
                                      : (std::min)(height, (std::max)(1u, controls.packedRows));
        {
            const D3D12_BOX box { 0, 0, 0, width, copyRows, 1 };
            D3D12_TEXTURE_COPY_LOCATION source {}, target {};
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.pResource = originalMotion;
            target.pResource = motion;
            command->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
            source.pResource = originalDepth;
            target.pResource = depth;
            command->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
        }
        if (motionState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalMotion, D3D12_RESOURCE_STATE_COPY_SOURCE, motionState);
        if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalDepth, D3D12_RESOURCE_STATE_COPY_SOURCE, depthState);

        transition(command, motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(command, depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(command, selection, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        struct Constants
        {
            unsigned width, height;
            float scaleX, scaleY;
            unsigned edgeWidth;
            float interiorStrength;
            unsigned debug, reserved;
        } constants { width, height, scaleX, scaleY, (std::min)(controls.edgeWidth, 4u), controls.coverage(),
                       dumpRequests.load(std::memory_order_relaxed) ? 1u : 0u, 0 };
        static_assert(sizeof(Constants) == 32);
        command->SetDescriptorHeaps(1, &heap);
        command->SetComputeRootSignature(root);
        command->SetComputeRootShaderResourceView(0, packed.resource->GetGPUVirtualAddress());
        command->SetComputeRootDescriptorTable(1, gpu(0));
        command->SetComputeRoot32BitConstants(2, 8, &constants, 0);
        command->SetPipelineState(pipeline);
        // The packed raster writes this buffer as a UAV/ROV during the draw, so
        // the compute read needs an explicit UAV barrier. Missing it is the
        // hazard that correlated with the 2026-09-14 driver resets.
        D3D12_RESOURCE_BARRIER packedBarrier {};
        packedBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        packedBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        packedBarrier.UAV.pResource = packed.resource;
        command->ResourceBarrier(1, &packedBarrier);
        // Coverage counters are per measured frame: reset, then let the shader
        // count dispatched, packed, edge and interior pixels.
        command->CopyBufferRegion(counters, 0, zeroCounters, 0, 64);
        D3D12_RESOURCE_BARRIER counterBarrier {};
        counterBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        counterBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        counterBarrier.UAV.pResource = counters;
        command->ResourceBarrier(1, &counterBarrier);
        // Staged coverage: run the real dispatch over the configured top rows so
        // a pathological cost cannot time out the GPU; the rest of the frame
        // keeps the copied original motion. Raise GlassFG/PackedRows after a
        // clean run; the INI and settings UI control this without a rebuild.
        const auto rows = (std::min)(height, (std::max)(1u, controls.packedRows));
        // Isolation: the copies and the swap stay, only the compute dispatch is
        // optional, so a reset can be attributed to the swap or to the compute.
        if (controls.packedCompute)
            command->Dispatch((width + 7) / 8, (rows + 7) / 8, 1);
        if (controls.trace && logFile)
        {
            std::fprintf(logFile, "TRACE_DISPATCH frame=%u rows=%u groups=%u edges=%u compute=%u copy=%u\n",
                         packed.frame, rows, (width + 7) / 8, (std::min)(controls.edgeWidth, 4u),
                         controls.packedCompute ? 1u : 0u, copyRows);
            std::fflush(logFile);
        }
        if (!dumpPending && dumpRequests.load(std::memory_order_relaxed) && prepareReadback())
        {
            transition(command, motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(command, depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ID3D12Resource* sources[] { motion, depth };
            for (unsigned i = 0; i < 2; ++i)
            {
                D3D12_TEXTURE_COPY_LOCATION source {}, target {};
                source.pResource = sources[i];
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                target.pResource = readback[i];
                target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                target.PlacedFootprint = readbackFootprint[i];
                command->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            }
            transition(command, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(command, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            command->CopyBufferRegion(readback[2], 0, counters, 0, 64);
            // Same-frame originals for a direct before/after comparison.
            if (motionState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                transition(command, originalMotion, motionState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                transition(command, originalDepth, depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ID3D12Resource* originals[] { originalMotion, originalDepth };
            for (unsigned i = 0; i < 2; ++i)
            {
                D3D12_TEXTURE_COPY_LOCATION source {}, target {};
                source.pResource = originals[i];
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                target.pResource = readback[3 + i];
                target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                target.PlacedFootprint = readbackFootprint[3 + i];
                command->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            }
            if (motionState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                transition(command, originalMotion, D3D12_RESOURCE_STATE_COPY_SOURCE, motionState);
            if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                transition(command, originalDepth, D3D12_RESOURCE_STATE_COPY_SOURCE, depthState);
            // Engine coverage: the packed object records themselves.
            transition(command, packed.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            command->CopyBufferRegion(readback[5], 0, packed.resource, 0, readbackBytes[5]);
            transition(command, packed.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            dumpFrame = packed.frame;
            dumpRequests.fetch_sub(1, std::memory_order_relaxed);
            dumpPending = true;
        }
        // The compose wrote the outputs as UAVs. Flush those writes explicitly
        // before the state transitions back: a transition out of
        // UNORDERED_ACCESS is not a UAV barrier on every driver, and this
        // hazard class already correlated with the 2026-09-14 driver resets
        // (see the packed-input barrier above).
        for (auto* resource : { motion, depth, selection })
            if (resource)
            {
                D3D12_RESOURCE_BARRIER outputBarrier {};
                outputBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                outputBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                outputBarrier.UAV.pResource = resource;
                command->ResourceBarrier(1, &outputBarrier);
            }
        transition(command, motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(command, depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        {
            const D3D12_BOX box { 0, 0, 0, width, height, 1 };
            D3D12_TEXTURE_COPY_LOCATION source {}, target {};
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.pResource = motion;
            target.pResource = motionRead;
            command->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
            source.pResource = depth;
            target.pResource = depthRead;
            command->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
        }
        transition(command, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(command, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(command, selection, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    }

    ID3D12Resource* motionOutput() const { return motionRead; }
    ID3D12Resource* depthOutput() const { return depthRead; }
    ID3D12Resource* selectionOutput() const { return selection; }
    void releaseAfterGpuDrain()
    {
        for (auto** resource : { &motion, &depth, &selection, &motionRead, &depthRead })
        {
            if (*resource)
                (*resource)->Release();
            *resource = nullptr;
        }
        for (auto** resource :
             { &readback[0], &readback[1], &readback[2], &readback[3], &readback[4], &readback[5], &counters,
               &zeroCounters })
        {
            if (*resource)
                (*resource)->Release();
            *resource = nullptr;
        }
        if (dumpFence) dumpFence->Release();
        dumpFence = nullptr;
        dumpPending = false;
        if (composeAllocator) composeAllocator->Release();
        if (composeList) composeList->Release();
        if (composeFence) composeFence->Release();
        composeAllocator = nullptr;
        composeList = nullptr;
        composeFence = nullptr;
        composeValue = 0;
        composePending = false;
        if (pipeline) pipeline->Release();
        if (root) root->Release();
        if (heap) heap->Release();
        pipeline = nullptr; root = nullptr; heap = nullptr; device = nullptr;
    }
};
} // namespace GlassFg
