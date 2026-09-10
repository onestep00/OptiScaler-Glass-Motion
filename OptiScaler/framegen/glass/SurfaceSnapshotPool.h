#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <limits>

namespace GlassFg
{
// The host serializes capture, evaluation, submission and Reset callbacks.
// Call afterSubmit AFTER the real ExecuteCommandLists and onReset AFTER a
// successful real Reset. Both must be observed before a recording can be reused.
// This pool records copies and completion signals, but never inserts waits.
// Cross-queue read ordering must already be established by SurfaceQueueLink.
class SurfaceSnapshotPool
{
  public:
    static constexpr unsigned Capacity = 4;
    struct Token
    {
        unsigned slot = Capacity;
        uint64_t generation = 0;
        explicit operator bool() const { return slot < Capacity && generation != 0; }
    };

  private:
    struct Slot
    {
        ID3D12Resource* texture = nullptr;
        ID3D12Resource* source = nullptr;
        ID3D12CommandList* writeCommand = nullptr;
        ID3D12CommandList* readCommand = nullptr;
        uint64_t generation = 0, writeFence = 0, readFence = 0;
        bool read = false, retired = false;
    };
    std::array<Slot, Capacity> slots {};
    ID3D12Fence* producerFence = nullptr;
    ID3D12Fence* consumerFence = nullptr;
    ID3D12CommandQueue* producer = nullptr;
    ID3D12CommandQueue* consumer = nullptr;
    uint64_t nextWrite = 0, nextRead = 0, latestGeneration = 0;
    UINT width = 0, height = 0;
    bool failed = false;

    Slot* find(Token token)
    {
        if (!token || slots[token.slot].generation != token.generation)
            return nullptr;
        return &slots[token.slot];
    }
    static void transition(ID3D12GraphicsCommandList* command, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        command->ResourceBarrier(1, &barrier);
    }
    bool completed(ID3D12Fence* fence, uint64_t value)
    {
        if (!value)
            return true; // Pending command references separately prevent reuse.
        const auto done = fence->GetCompletedValue();
        if (done == std::numeric_limits<uint64_t>::max())
        {
            failed = true;
            return false;
        }
        return done >= value;
    }
    bool reusable(const Slot& slot)
    {
        return !slot.generation ||
               (slot.retired && !slot.writeCommand && !slot.readCommand && completed(producerFence, slot.writeFence) &&
                completed(consumerFence, slot.readFence));
    }
    static bool contains(UINT count, ID3D12CommandList* const* commands, ID3D12CommandList* wanted)
    {
        if (!wanted)
            return false;
        for (UINT i = 0; i < count; ++i)
            if (commands[i] == wanted)
                return true;
        return false;
    }

  public:
    SurfaceSnapshotPool() = default;
    SurfaceSnapshotPool(const SurfaceSnapshotPool&) = delete;
    SurfaceSnapshotPool& operator=(const SurfaceSnapshotPool&) = delete;

    bool initialize(ID3D12Device* device, UINT textureWidth, UINT textureHeight)
    {
        if (!device || producerFence || failed || !textureWidth || !textureHeight || textureWidth > 3840 ||
            textureHeight > 2160)
            return false;
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = textureWidth;
        desc.Height = textureHeight;
        desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        bool ok = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producerFence))) &&
                  SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumerFence)));
        for (auto& slot : slots)
            if (ok)
                ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                               IID_PPV_ARGS(&slot.texture)));
        if (!ok)
        {
            releaseAfterGpuDrain(); // No commands have been recorded yet.
            failed = true;
            return false;
        }
        width = textureWidth;
        height = textureHeight;
        return true;
    }

    // Called after the identified non-split DEPTH_WRITE -> read-state barrier.
    // COMMON is the snapshot's boundary state on both queues. The original
    // depth is restored to its exact read state; root/descriptor state is unused.
    Token capture(ID3D12GraphicsCommandList* command, ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState,
                  uint64_t generation)
    {
        constexpr auto ExpectedState = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (failed || !producerFence || !command || !source || !generation || generation <= latestGeneration ||
            sourceState != ExpectedState || command->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return {};
        const auto desc = source->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != width || desc.Height != height ||
            desc.Format != DXGI_FORMAT_R32_TYPELESS || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
            desc.SampleDesc.Count != 1 || !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
            return {};
        for (unsigned i = 0; i < Capacity; ++i)
        {
            auto& slot = slots[i];
            if (!reusable(slot) || failed)
                continue;
            if (slot.source)
                slot.source->Release();
            auto* texture = slot.texture;
            slot = {};
            slot.texture = texture;
            slot.source = source;
            source->AddRef();
            slot.writeCommand = command;
            slot.generation = generation;
            latestGeneration = generation;
            transition(command, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(command, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            command->CopyResource(texture, source);
            transition(command, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            transition(command, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
            return { i, generation };
        }
        return {}; // No CPU stall and no GPU commands when the pool is full.
    }

    // Reserve once immediately BEFORE recording the phase-1 copy. The caller
    // supplies the exact generation validated by the native queue dependency.
    // Later FG phases reuse Pass outputs; they must not read this snapshot again.
    ID3D12Resource* beginRead(Token token, ID3D12GraphicsCommandList* command, uint64_t orderedGeneration)
    {
        auto* slot = find(token);
        if (failed || !slot || slot->retired || slot->read || !slot->writeFence || !command ||
            orderedGeneration != token.generation || command->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE)
            return nullptr;
        slot->read = true;
        slot->readCommand = command;
        return slot->texture;
    }

    void retire(Token token)
    {
        if (auto* slot = find(token))
            slot->retired = true;
    }

    // Repeated submissions of the same closed recording update its completion
    // point. Never release the recording association merely upon submission.
    bool afterSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* commands)
    {
        if (failed || !producerFence || !queue || !commands || !count)
            return false;
        bool writes = false, reads = false;
        for (const auto& slot : slots)
        {
            writes |= contains(count, commands, slot.writeCommand);
            reads |= contains(count, commands, slot.readCommand);
        }
        if (!writes && !reads)
            return true;
        if ((writes && ((producer && producer != queue) || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)) ||
            (reads && ((consumer && consumer != queue) || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)))
        {
            failed = true;
            return false;
        }
        if (writes)
        {
            if (!producer)
            {
                producer = queue;
                producer->AddRef();
            }
            if (FAILED(queue->Signal(producerFence, ++nextWrite)))
            {
                failed = true;
                return false;
            }
        }
        if (reads)
        {
            if (!consumer)
            {
                consumer = queue;
                consumer->AddRef();
            }
            if (FAILED(queue->Signal(consumerFence, ++nextRead)))
            {
                failed = true;
                return false;
            }
        }
        for (auto& slot : slots)
        {
            if (contains(count, commands, slot.writeCommand))
                slot.writeFence = nextWrite;
            if (contains(count, commands, slot.readCommand))
                slot.readFence = nextRead;
        }
        return true;
    }

    // Reset or proven command destruction discards the recording association.
    // Identity comparison only; the object may already be dead.
    void discardRecording(const void* command)
    {
        if (!command)
            return;
        for (auto& slot : slots)
        {
            if (slot.writeCommand == command)
            {
                slot.writeCommand = nullptr;
                if (!slot.writeFence)
                    slot.retired = true; // Discarded recording, never submitted.
            }
            if (slot.readCommand == command)
                slot.readCommand = nullptr;
        }
    }

    void onReset(ID3D12GraphicsCommandList* command) { discardRecording(command); }

    bool healthy() const { return producerFence && !failed; }

    // The host must first stop capture/read admission. Completion alone is not
    // enough: a still-closed command list could submit this recording again.
    bool idle()
    {
        if (failed)
            return false;
        for (const auto& slot : slots)
            if (slot.writeCommand || slot.readCommand || !completed(producerFence, slot.writeFence) ||
                !completed(consumerFence, slot.readFence))
                return false;
        return true;
    }

    // The caller must stop new recordings, discard outstanding unsubmitted
    // lists, and drain both queues. No COM release is hidden in a destructor.
    void releaseAfterGpuDrain()
    {
        for (auto& slot : slots)
        {
            if (slot.source)
                slot.source->Release();
            if (slot.texture)
                slot.texture->Release();
            slot = {};
        }
        if (producer)
            producer->Release();
        if (consumer)
            consumer->Release();
        if (producerFence)
            producerFence->Release();
        if (consumerFence)
            consumerFence->Release();
        producer = consumer = nullptr;
        producerFence = consumerFence = nullptr;
        width = height = 0;
        nextWrite = nextRead = latestGeneration = 0;
    }
};
} // namespace GlassFg
