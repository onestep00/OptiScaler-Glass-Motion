#pragma once
#include "ExperimentRuntime.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace GlassFg
{
// One actual command recording, identified by a host-issued non-reused epoch.
// The host serializes these notifications. It calls submitted only after the
// real queue signal, and discard only after successful Reset/destruction.
class ExperimentRecording
{
    ExperimentRuntime::Lease lease;
    const void* command = nullptr;
    uint64_t epoch = 0;
    struct Completion
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UINT64 value = 0;
    };
    std::array<Completion, 4> completions;
    unsigned used = 0;
    bool discarded = false, uncertain = false;
  public:
    ExperimentRecording(ExperimentRuntime::Lease value, const void* list, uint64_t recording)
        : lease(std::move(value)), command(list), epoch(recording), uncertain(!list || !recording) {}
    ExperimentRecording(const ExperimentRecording&) = delete;
    ExperimentRecording& operator=(const ExperimentRecording&) = delete;
    ~ExperimentRecording()
    {
        // Destruction is not GPU completion or permission to unload the DLL.
        if (lease)
        {
            struct Retained
            {
                ExperimentRuntime::Lease module;
                std::array<Completion, 4> fences;
            };
            (void)new Retained { std::move(lease), std::move(completions) };
        }
    }
    bool submitted(ID3D12Fence* fence, UINT64 value)
    {
        if (!lease || discarded || !fence || !value || value == UINT64_MAX || used == completions.size())
        {
            uncertain = true;
            return false;
        }
        completions[used++] = { fence, value };
        return true;
    }
    void submissionUnknown() { uncertain = true; }
    bool discard(const void* list, uint64_t recording)
    {
        if (list != command || recording != epoch) return false;
        discarded = true;
        return true;
    }
    bool retire()
    {
        if (!lease || uncertain || !discarded) return false;
        for (unsigned i = 0; i < used; ++i)
        {
            const auto done = completions[i].fence->GetCompletedValue();
            if (done == UINT64_MAX || done < completions[i].value) return false;
        }
        lease = {};
        for (auto& completion : completions) completion = {};
        return true;
    }
};
} // namespace GlassFg
