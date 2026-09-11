// Exercise the production callbacks with owned layouts and original-function
// substitutes. No target executable, live process, or engine hook is installed.
#include "../CyberpunkObjects.cpp"
#include <cstdio>

namespace
{
std::uint32_t tick = 10;
bool nextResult = true, crossFrame = false, retireBeforeOriginal = false;
GlassFg::State* fixture = nullptr;
std::array<unsigned char, 0x1c0> object {};
GlassFg::GeometryObjectPose current;
void require(bool okay, const char* text)
{
    if (!okay)
        throw std::runtime_error(text);
}
bool originalRegister(void*) { return nextResult; }
void originalRemove(void* proxy)
{
    retireBeforeOriginal = !fixture->registry->ticket(reinterpret_cast<std::uint64_t>(proxy), 2);
    const auto absent = UINT32_MAX;
    memcpy(static_cast<char*>(proxy) + 0x98, &absent, 4);
}
bool originalUpdate(void* proxy, void* bounds, void* packed)
{
    memcpy(static_cast<char*>(proxy) + 0x18, packed, 48);
    memcpy(static_cast<char*>(proxy) + 0x50, bounds, 24);
    if (crossFrame)
        ++tick;
    return nextResult;
}
void prepare()
{
    const std::uint64_t mesh = 500;
    const std::uint32_t index = 2;
    current.packed = { 0x3f800000, 0, 0, 0, 0, 0x3f800000, 0, 0, 0, 0, 0x3f800000, 0 };
    current.bounds = { -1, -1, -1, 1, 1, 1 };
    current.frame = tick;
    memcpy(object.data() + 0x18, current.packed.data(), 48);
    memcpy(object.data() + 0x50, current.bounds.data(), 24);
    memcpy(object.data() + 0x98, &index, 4);
    memcpy(object.data() + 0xd8, &mesh, 8);
}
} // namespace
int main()
{
    try
    {
        GlassFg::State state;
        fixture = &state;
        state.tick = &tick;
        state.registry = std::make_shared<GlassFg::GeometryObjectRegistry>(8);
        GlassFg::originalRegister = &originalRegister;
        GlassFg::originalRemove = &originalRemove;
        GlassFg::originalUpdate = &originalUpdate;
        GlassFg::activeState.store(&state);
        struct Stop
        {
            ~Stop() { GlassFg::activeState.store(nullptr); }
        } stop;
        prepare();
        const auto proxy = reinterpret_cast<std::uint64_t>(object.data());
        nextResult = false;
        require(!GlassFg::registered(object.data()) && !state.registry->ticket(proxy, 2),
                "Failed original registration changed");
        nextResult = true;
        require(GlassFg::registered(object.data()), "Successful original registration changed");
        auto first = state.registry->find(500, current.packed, tick);
        require(first && first.proxy == proxy && first.pose.bounds == current.bounds, "Original proxy layout snapshot");
        current.packed[3] = 131072;
        nextResult = false;
        require(!GlassFg::updated(object.data(), current.bounds.data(), current.packed.data()),
                "Original update result changed");
        require(state.registry->find(500, current.packed, tick).generation == first.generation,
                "Post-update pose not copied");
        crossFrame = true;
        current.packed[3] += 131072;
        GlassFg::updated(object.data(), current.bounds.data(), current.packed.data());
        require(!state.registry->find(500, current.packed, tick) && state.rejected == 1,
                "Mixed frame was not invalidated");
        require(state.registry->ticket(proxy, 2) != first.generation, "Mixed frame retained old generation");
        crossFrame = false;
        GlassFg::updated(object.data(), current.bounds.data(), current.packed.data());
        auto recovered = state.registry->find(500, current.packed, tick);
        require(recovered && recovered.generation != first.generation, "Valid later update failed to recover");
        GlassFg::removed(object.data());
        require(retireBeforeOriginal && !state.registry->ticket(proxy, 2), "Removal ordering");
        prepare();
        nextResult = true;
        require(GlassFg::registered(object.data()) && state.registry->ticket(proxy, 2) != recovered.generation,
                "Pointer reuse");
        void* unreadable = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        require(unreadable != nullptr, "Protected-page fixture allocation");
        const bool forwarded = GlassFg::registered(unreadable);
        VirtualFree(unreadable, 0, MEM_RELEASE);
        require(forwarded && state.rejected == 2, "Unreadable snapshot changed original result");
        printf("PASS engine_callbacks=1 original_results_preserved=1 actual_layout_copied=1 mixed_frame_rejected=1 "
               "later_recovery=1 retire_before_free=1 pointer_reuse=1 unreadable_memory_rejected=1 "
               "game_hooks_installed=0\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
