#pragma once
#include <d3d12.h>
#include <d3dcommon.h>
#include <atomic>
#include <memory>
#include <new>

namespace GlassFg
{
// No reference is held on the command list. A self-contained callback token can
// outlive the session, and only publishes a flag; it never accesses the dying
// COM object, acquires the host lock or touches GPU resources.
class CommandLifetime
{
    struct State
    {
        const void* identity = nullptr;
        std::atomic<bool> destroyed = false;
        UINT callback = 0;
        std::shared_ptr<State>* token = nullptr;
    };
    std::shared_ptr<State> state;

    static void CALLBACK destroyed(void* data)
    {
        auto* token = static_cast<std::shared_ptr<State>*>(data);
        (*token)->destroyed.store(true, std::memory_order_release);
        delete token;
    }

  public:
    CommandLifetime() = default;
    CommandLifetime(const CommandLifetime&) = delete;
    CommandLifetime& operator=(const CommandLifetime&) = delete;

    bool attach(ID3D12GraphicsCommandList* command)
    {
        if (!command || state)
            return false;
        std::shared_ptr<State> candidate;
        try
        {
            candidate = std::make_shared<State>();
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        ID3DDestructionNotifier* notifier = nullptr;
        if (FAILED(command->QueryInterface(IID_PPV_ARGS(&notifier))))
            return false;
        candidate->identity = command;
        auto* token = new (std::nothrow) std::shared_ptr<State>(candidate);
        if (!token)
        {
            notifier->Release();
            return false;
        }
        candidate->token = token;
        const auto result = notifier->RegisterDestructionCallback(destroyed, token, &candidate->callback);
        notifier->Release();
        if (FAILED(result))
        {
            delete token;
            return false;
        }
        state = std::move(candidate);
        return true;
    }

    const void* identity() const { return state ? state->identity : nullptr; }
    bool wasDestroyed() const { return state && state->destroyed.load(std::memory_order_acquire); }
    const void* takeDestroyed()
    {
        if (!state || !state->destroyed.load(std::memory_order_acquire))
            return nullptr;
        const auto identity = state->identity;
        state.reset();
        return identity;
    }

    // Only from a real callback whose caller guarantees command is still alive,
    // e.g. successful Reset. Never query a stored pointer just because its last
    // observed destroyed flag was false.
    bool detachLive(ID3D12GraphicsCommandList* command)
    {
        if (!state)
            return true;
        if (state->identity != command || state->destroyed.load(std::memory_order_acquire))
            return false;
        ID3DDestructionNotifier* notifier = nullptr;
        if (FAILED(command->QueryInterface(IID_PPV_ARGS(&notifier))))
            return false;
        const auto result = notifier->UnregisterDestructionCallback(state->callback);
        notifier->Release();
        if (FAILED(result))
            return false;
        delete state->token;
        state.reset();
        return true;
    }

    // No COM call. A still-registered callback keeps only its small CPU state,
    // and frees it when the object dies. This does not prove GPU retirement.
    void forget() { state.reset(); }
};
} // namespace GlassFg
