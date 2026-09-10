#pragma once
#include <d3d12.h>
#include <cstdint>

namespace GlassFg
{
struct D3D12Callbacks
{
    void* context = nullptr;
    void (*enter)(void*) = nullptr;
    void (*leave)(void*) = nullptr;
    // Optional lock-free identity predicate, called only for binding mutations.
    bool (*stateTracked)(void*, ID3D12GraphicsCommandList*) = nullptr;
    void (*reset)(void*, ID3D12GraphicsCommandList*, bool, ID3D12PipelineState*) = nullptr;
    void (*mutation)(void*, ID3D12GraphicsCommandList*) = nullptr;
    void (*barrier)(void*, ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*) = nullptr;
    void (*submit)(void*, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) = nullptr;
    void (*signal)(void*, ID3D12CommandQueue*, ID3D12Fence*, UINT64) = nullptr;
    void (*wait)(void*, ID3D12CommandQueue*, ID3D12Fence*, UINT64) = nullptr;
};

// One process-lifetime observer. Callbacks and their context must remain alive;
// they can become no-ops after session retirement. The original calls execute
// inside enter/leave, with only this module's nested commands excluded.
bool InstallD3D12Observer(ID3D12GraphicsCommandList* actualFgCommand, const D3D12Callbacks& callbacks);
bool MatchesD3D12Observer(ID3D12GraphicsCommandList* command);
uint32_t ObservedComputeMethods();

class InternalD3D12Scope
{
  public:
    InternalD3D12Scope();
    ~InternalD3D12Scope();
    InternalD3D12Scope(const InternalD3D12Scope&) = delete;
    InternalD3D12Scope& operator=(const InternalD3D12Scope&) = delete;
};
} // namespace GlassFg
