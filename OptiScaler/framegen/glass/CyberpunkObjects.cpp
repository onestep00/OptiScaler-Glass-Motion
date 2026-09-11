#include "pch.h"
#include "CyberpunkObjects.h"
#include "CyberpunkSurfacePass.h"
#include "DetourThreads.h"

namespace GlassFg
{
namespace
{
using Register = bool (*)(void*);
using Remove = void (*)(void*);
using Update = bool (*)(void*, void*, void*);
Register originalRegister = nullptr;
Remove originalRemove = nullptr;
Update originalUpdate = nullptr;
struct State
{
    std::shared_ptr<GeometryObjectRegistry> registry;
    const volatile std::uint32_t* tick = nullptr;
    std::atomic<std::uint64_t> rejected = 0;
};
std::atomic<State*> activeState = nullptr;
struct Snapshot
{
    std::uint64_t mesh = 0;
    std::uint32_t index = UINT32_MAX;
    GeometryObjectPose pose;
};
bool slot(void* proxy, std::uint32_t& index) noexcept
{
    __try
    {
        if (!proxy)
            return false;
        memcpy(&index, static_cast<const char*>(proxy) + 0x98, sizeof(index));
        return index < 131072;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
bool snapshot(State& state, void* proxy, Snapshot& output) noexcept
{
    // Called while an actual engine method owns its proxy argument. The second
    // copy rejects a concurrently changing header/pose. It is not a GPU readback.
    __try
    {
        Snapshot first, second;
        const auto copy = [&](Snapshot& value)
        {
            value.pose.frame = *state.tick;
            memcpy(&value.mesh, static_cast<const char*>(proxy) + 0xd8, 8);
            memcpy(&value.index, static_cast<const char*>(proxy) + 0x98, 4);
            memcpy(value.pose.packed.data(), static_cast<const char*>(proxy) + 0x18, 48);
            memcpy(value.pose.bounds.data(), static_cast<const char*>(proxy) + 0x50, 24);
        };
        copy(first);
        MemoryBarrier();
        copy(second);
        if (first.mesh != second.mesh || first.index != second.index || first.pose.frame != second.pose.frame ||
            first.pose.packed != second.pose.packed || first.pose.bounds != second.pose.bounds || !first.mesh ||
            first.index >= 131072 || !first.pose.valid())
            return false;
        output = first;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
bool registered(void* proxy)
{
    const bool result = originalRegister(proxy);
    if (result)
        if (auto* state = activeState.load(std::memory_order_acquire))
        {
            try
            {
                Snapshot value;
                if (snapshot(*state, proxy, value))
                    state->registry->registered(reinterpret_cast<std::uint64_t>(proxy), value.index, value.mesh,
                                                value.pose);
                else
                {
                    std::uint32_t index;
                    if (slot(proxy, index))
                        state->registry->pending(reinterpret_cast<std::uint64_t>(proxy), index);
                    ++state->rejected;
                }
            }
            catch (...)
            {
                activeState.store(nullptr, std::memory_order_release);
            }
        }
    return result;
}
void removed(void* proxy)
{
    if (auto* state = activeState.load(std::memory_order_acquire))
    {
        try
        {
            std::uint32_t index;
            if (slot(proxy, index))
                state->registry->removed(reinterpret_cast<std::uint64_t>(proxy), index);
        }
        catch (...)
        {
            activeState.store(nullptr, std::memory_order_release);
        }
    }
    // Retire before the original frees the registry slot; copied query results
    // contain no borrowed engine storage and cannot dereference a dead proxy.
    originalRemove(proxy);
}
bool updated(void* proxy, void* bounds, void* transform)
{
    auto* state = activeState.load(std::memory_order_acquire);
    std::uint32_t index = UINT32_MAX, generation = 0, before = 0;
    if (state && slot(proxy, index))
    {
        try
        {
            generation = state->registry->ticket(reinterpret_cast<std::uint64_t>(proxy), index);
            if (generation)
                before = *state->tick;
        }
        catch (...)
        {
            activeState.store(nullptr, std::memory_order_release);
        }
    }
    const bool result = originalUpdate(proxy, bounds, transform);
    if (generation)
    {
        try
        {
            Snapshot value;
            if (snapshot(*state, proxy, value) && value.pose.frame == before && value.index == index)
                state->registry->update(reinterpret_cast<std::uint64_t>(proxy), index, generation, value.mesh,
                                        value.pose);
            else
            {
                state->registry->invalidate(reinterpret_cast<std::uint64_t>(proxy), index, generation);
                ++state->rejected;
            }
        }
        catch (...)
        {
            activeState.store(nullptr, std::memory_order_release);
        }
    }
    return result;
}
bool fingerprint(const unsigned char* code, unsigned bytes, std::uint64_t expected)
{
    std::uint64_t value = 14695981039346656037ull;
    for (unsigned i = 0; i < bytes; ++i)
        value = (value ^ code[i]) * 1099511628211ull;
    return value == expected;
}
} // namespace

bool InitializeCyberpunkObjects(HMODULE executable) noexcept
{
    try
    {
        static std::mutex startup;
        std::lock_guard lock(startup);
        if (activeState.load(std::memory_order_acquire))
            return true;
        CyberpunkSurfacePass admitted;
        if (!admitted.initialize(executable, nullptr))
            return false;
        const auto* base = reinterpret_cast<const unsigned char*>(executable);
        // Full small-function fingerprints of the inspected game build. No
        // absolute heap addresses, names, or driver-code signatures are used.
        if (!fingerprint(base + 0x3a1ca4, 161, 0xd5773b1945057908ull) ||
            !fingerprint(base + 0x294724, 59, 0x41f8f549e402f483ull) ||
            !fingerprint(base + 0x1da094, 521, 0x3fb3d88b769d5d1full))
            return false;
        auto state = std::make_unique<State>();
        state->registry = std::make_shared<GeometryObjectRegistry>();
        state->tick = reinterpret_cast<const volatile std::uint32_t*>(base + 0x3438a30);
        HMODULE resident = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&InitializeCyberpunkObjects), &resident))
            return false;
        DetourThreads threads;
        if (!threads.gather() || DetourTransactionBegin() != NO_ERROR)
            return false;
        originalRegister = reinterpret_cast<Register>(const_cast<unsigned char*>(base + 0x3a1ca4));
        originalRemove = reinterpret_cast<Remove>(const_cast<unsigned char*>(base + 0x294724));
        originalUpdate = reinterpret_cast<Update>(const_cast<unsigned char*>(base + 0x1da094));
        bool okay =
            DetourAttach(reinterpret_cast<PVOID*>(&originalRegister), reinterpret_cast<PVOID>(&registered)) == NO_ERROR;
        okay = DetourAttach(reinterpret_cast<PVOID*>(&originalRemove), reinterpret_cast<PVOID>(&removed)) == NO_ERROR &&
               okay;
        okay = DetourAttach(reinterpret_cast<PVOID*>(&originalUpdate), reinterpret_cast<PVOID>(&updated)) == NO_ERROR &&
               okay;
        if (!okay || !threads.enlist())
        {
            DetourTransactionAbort();
            return false;
        }
        // The process-resident state is published before releasing suspended
        // engine threads. On commit failure no detour became visible.
        activeState.store(state.get(), std::memory_order_release);
        if (DetourTransactionCommit() != NO_ERROR)
        {
            activeState.store(nullptr, std::memory_order_release);
            return false;
        }
        state.release();
        return true;
    }
    catch (...)
    {
        return false;
    }
}
std::shared_ptr<GeometryObjectRegistry> GetCyberpunkObjects() noexcept
{
    auto* state = activeState.load(std::memory_order_acquire);
    return state ? state->registry : nullptr;
}
CyberpunkObjectStatus GetCyberpunkObjectStatus()
{
    auto* state = activeState.load(std::memory_order_acquire);
    return state ? CyberpunkObjectStatus { true, state->rejected.load(), state->registry->stats() }
                 : CyberpunkObjectStatus {};
}
} // namespace GlassFg
