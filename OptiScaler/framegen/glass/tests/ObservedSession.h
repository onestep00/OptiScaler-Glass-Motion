#pragma once
#include "../D3D12Observer.cpp"
#include <mutex>

// Test-only adapter: issue real API calls and let the production observer drive
// NativeSession. The test surface is explicit; game pass identification is not
// exercised by this independent device test.
namespace ObserverTest
{
struct Context
{
    std::recursive_mutex mutex;
    GlassFg::NativeSession* session = nullptr;
    ID3D12Resource* surface = nullptr;
    bool omitWait = false, okay = true, captured = false;
    unsigned resets = 0, mutations = 0, barriers = 0, submits = 0, signals = 0, waits = 0;
    unsigned preparedSubmits = 0, preparedCount = 0;
    bool submitOrderOkay = true;
    ID3D12CommandQueue* preparedQueue = nullptr;
    ID3D12CommandList* const* preparedLists = nullptr;
};
inline Context& context()
{
    static auto* value = new Context;
    return *value;
}
inline void omitWait(bool value) { context().omitWait = value; }
inline GlassFg::D3D12Callbacks callbacks()
{
    GlassFg::D3D12Callbacks result;
    result.context = &context();
    result.enter = [](void* p) { static_cast<Context*>(p)->mutex.lock(); };
    result.leave = [](void* p) { static_cast<Context*>(p)->mutex.unlock(); };
    result.reset = [](void* p, ID3D12GraphicsCommandList* c, bool success, ID3D12PipelineState* initial)
    {
        auto& x = *static_cast<Context*>(p);
        ++x.resets;
        if (x.session)
            x.session->onReset(c, success, initial);
    };
    result.mutation = [](void* p, ID3D12GraphicsCommandList* c)
    {
        auto& x = *static_cast<Context*>(p);
        ++x.mutations;
        if (x.session)
            x.session->onStateMutation(c);
    };
    result.barrier = [](void* p, ID3D12GraphicsCommandList* c, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
    {
        auto& x = *static_cast<Context*>(p);
        ++x.barriers;
        if (!x.session || c->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return;
        for (UINT i = 0; i < count; ++i)
        {
            const auto& b = barriers[i];
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource == x.surface &&
                b.Transition.StateBefore == D3D12_RESOURCE_STATE_DEPTH_WRITE)
            {
                GlassFg::InternalD3D12Scope ownCommands;
                x.captured = x.session->captureIdentifiedSurface(c, x.surface, b.Transition.StateAfter);
            }
        }
    };
    result.beforeSubmit = [](void* p, ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
    {
        auto& x = *static_cast<Context*>(p);
        x.submitOrderOkay &= x.preparedSubmits == x.submits;
        ++x.preparedSubmits;
        x.preparedQueue = q; x.preparedCount = count; x.preparedLists = lists;
    };
    result.submit = [](void* p, ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
    {
        auto& x = *static_cast<Context*>(p);
        ++x.submits;
        x.submitOrderOkay &= x.preparedSubmits == x.submits && x.preparedQueue == q &&
                  x.preparedCount == count && x.preparedLists == lists;
        if (x.session)
        {
            GlassFg::InternalD3D12Scope ownSignals;
            x.okay &= x.session->afterSubmit(q, count, lists);
        }
    };
    result.signal = [](void* p, ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 value)
    {
        auto& x = *static_cast<Context*>(p);
        ++x.signals;
        if (x.session)
            x.session->onSignal(q, f, value);
    };
    result.wait = [](void* p, ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 value)
    {
        auto& x = *static_cast<Context*>(p);
        ++x.waits;
        if (x.session && !x.omitWait)
            x.session->onWait(q, f, value);
    };
    return result;
}
class Session : public GlassFg::NativeSession
{
  public:
    bool initialize(ID3D12Device* device, const D3D12_RESOURCE_DESC (&descs)[3], const wchar_t* seed,
                    const wchar_t* region, FILE* log)
    {
        GlassFg::InternalD3D12Scope ownCommands;
        auto okay = NativeSession::initialize(device, descs, seed, region, log);
        context().session = okay ? this : nullptr;
        context().okay = okay;
        return okay;
    }
    bool bindFgCommand(ID3D12GraphicsCommandList* command, uint32_t, uint32_t)
    {
        if (!GlassFg::InstallD3D12Observer(command, callbacks()))
            return false;
        auto mask = GlassFg::ObservedComputeMethods();
        return NativeSession::bindFgCommand(command, mask, mask);
    }
    bool captureIdentifiedSurface(ID3D12GraphicsCommandList* command, ID3D12Resource* depth,
                                  D3D12_RESOURCE_STATES state)
    {
        context().surface = depth;
        context().captured = false;
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { depth, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, D3D12_RESOURCE_STATE_DEPTH_WRITE };
        command->ResourceBarrier(1, &b);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        command->ResourceBarrier(1, &b);
        return context().captured;
    }
    GlassFg::PreparedInputs prepare(ID3D12GraphicsCommandList* command, const GlassFg::Inputs& inputs,
                                    const D3D12_RESOURCE_STATES (&states)[3], GlassFg::Controls controls)
    {
        const auto mutations = context().mutations;
        GlassFg::InternalD3D12Scope ownCommands;
        auto value = NativeSession::prepare(command, inputs, states, controls);
        if (context().mutations != mutations)
            throw std::runtime_error("own commands entered state observer");
        return value;
    }
    // Existing test sites explicitly notify the base session in manual mode.
    // Here those notifications are supplied exclusively by actual API hooks.
    void onReset(ID3D12GraphicsCommandList*, bool, ID3D12PipelineState*) {}
    void onStateMutation(ID3D12GraphicsCommandList*) {}
    void onSignal(ID3D12CommandQueue*, ID3D12Fence*, uint64_t) {}
    void onWait(ID3D12CommandQueue*, ID3D12Fence*, uint64_t) {}
    bool afterSubmit(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) { return context().okay; }
    void releaseAfterGpuDrain()
    {
        context().session = nullptr;
        GlassFg::InternalD3D12Scope ownCommands;
        NativeSession::releaseAfterGpuDrain();
    }
};
} // namespace ObserverTest
