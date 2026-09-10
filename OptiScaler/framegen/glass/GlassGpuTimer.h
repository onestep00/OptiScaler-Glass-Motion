#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace GlassFg
{
// Optional, sparse GPU timing. The host already owns a monotonic completion
// fence; this helper never inserts Signal/Wait or blocks the CPU. Serialize its
// callbacks with the host's submission/Reset tracking. Keep it per FG queue.
class GpuTimer
{
  public:
    static constexpr unsigned Capacity = 8;
    struct Ticket
    {
        unsigned slot = Capacity;
        uint64_t sequence = 0;
        explicit operator bool() const { return slot < Capacity && sequence; }
    };
    struct Sample
    {
        double milliseconds;
        uint64_t sequence;
    };

  private:
    struct Slot
    {
        ID3D12GraphicsCommandList* command = nullptr;
        ID3D12Fence* fence = nullptr;
        uint64_t completion = 0, sequence = 0;
        bool ended = false, delivered = false;
    };
    std::array<Slot, Capacity> slots {};
    ID3D12QueryHeap* heap = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    const unsigned char* mapped = nullptr;
    uint64_t frequency = 0, nextSequence = 0;
    bool failed = false;

  public:
    GpuTimer() = default;
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    bool initialize(ID3D12Device* device)
    {
        if (!device || heap || failed)
            return false;
        D3D12_QUERY_HEAP_DESC query {};
        query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query.Count = Capacity * 2;
        D3D12_HEAP_PROPERTIES properties {};
        properties.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = Capacity * 2 * sizeof(uint64_t);
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* data = nullptr;
        const D3D12_RANGE range { 0, static_cast<SIZE_T>(desc.Width) };
        if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&heap))) ||
            FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
            FAILED(readback->Map(0, &range, &data)))
        {
            releaseAfterGpuDrain();
            failed = true;
            return false;
        }
        mapped = static_cast<const unsigned char*>(data);
        return true;
    }

    // Call only on sampled rendered frames, before input copies and correction.
    // Do not time each MFG phase or include the provider's FG evaluation here.
    Ticket begin(ID3D12GraphicsCommandList* command)
    {
        if (failed || !mapped || !command || command->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE)
            return {};
        for (unsigned i = 0; i < Capacity; ++i)
            if (!slots[i].sequence)
            {
                slots[i].command = command;
                slots[i].sequence = ++nextSequence;
                command->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, i * 2);
                return { i, nextSequence };
            }
        return {}; // In-flight measurements never force a wait.
    }

    void end(ID3D12GraphicsCommandList* command, Ticket ticket)
    {
        if (!ticket || failed)
            return;
        auto& slot = slots[ticket.slot];
        if (slot.sequence != ticket.sequence || slot.command != command || slot.ended)
            return;
        command->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, ticket.slot * 2 + 1);
        command->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, ticket.slot * 2, 2, readback,
                                  ticket.slot * 2 * sizeof(uint64_t));
        slot.ended = true;
    }

    // Fence/value must be a successful GPU Signal AFTER this real submission,
    // from a host-owned monotonic timeline on this same queue, never a CPU signal.
    bool submitted(ID3D12GraphicsCommandList* command, ID3D12CommandQueue* submittedQueue, ID3D12Fence* fence,
                   uint64_t value)
    {
        if (failed || !heap || !command || !submittedQueue || !fence || !value)
            return false;
        bool relevant = false;
        for (const auto& slot : slots)
            relevant |= slot.command == command;
        if (!relevant)
            return true;
        if ((queue && queue != submittedQueue) || submittedQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        {
            failed = true;
            return false;
        }
        if (!queue)
        {
            if (FAILED(submittedQueue->GetTimestampFrequency(&frequency)) || !frequency)
            {
                failed = true;
                return false;
            }
            queue = submittedQueue;
            queue->AddRef();
        }
        for (auto& slot : slots)
            if (slot.command == command)
            {
                fence->AddRef();
                if (slot.fence)
                    slot.fence->Release();
                slot.fence = fence;
                slot.completion = value;
                slot.delivered = false;
            }
        return true;
    }

    // Identity-only bookkeeping; safe after proven command destruction too.
    void discardRecording(const void* command)
    {
        for (auto& slot : slots)
            if (slot.command == command)
            {
                slot.command = nullptr;
                if (!slot.fence)
                    slot = {}; // The recording was discarded without submission.
            }
    }

    void onReset(ID3D12GraphicsCommandList* command) { discardRecording(command); }

    std::optional<Sample> poll()
    {
        if (failed || !mapped)
            return {};
        std::optional<Sample> latest;
        for (unsigned i = 0; i < Capacity; ++i)
        {
            auto& slot = slots[i];
            if (!slot.fence)
                continue;
            const auto completed = slot.fence->GetCompletedValue();
            if (completed == std::numeric_limits<uint64_t>::max())
            {
                failed = true;
                return {};
            }
            if (completed < slot.completion)
                continue;
            if (!slot.delivered && slot.ended)
            {
                uint64_t ticks[2];
                std::memcpy(ticks, mapped + i * sizeof(ticks), sizeof(ticks));
                if (ticks[1] >= ticks[0] && (!latest || latest->sequence < slot.sequence))
                    latest = Sample { (ticks[1] - ticks[0]) * 1000.0 / static_cast<double>(frequency), slot.sequence };
                slot.delivered = true;
            }
            if (!slot.command)
            {
                slot.fence->Release();
                slot = {};
            }
        }
        return latest;
    }

    // Stop recording, discard pending lists and drain submitted GPU work first.
    void releaseAfterGpuDrain()
    {
        for (auto& slot : slots)
        {
            if (slot.fence)
                slot.fence->Release();
            slot = {};
        }
        if (readback)
        {
            if (mapped)
            {
                const D3D12_RANGE written { 0, 0 };
                readback->Unmap(0, &written);
            }
            readback->Release();
        }
        if (heap)
            heap->Release();
        if (queue)
            queue->Release();
        readback = nullptr;
        heap = nullptr;
        queue = nullptr;
        mapped = nullptr;
        frequency = nextSequence = 0;
    }
};
} // namespace GlassFg
