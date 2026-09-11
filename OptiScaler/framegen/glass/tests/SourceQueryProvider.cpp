// Independent fixture DLL only. Never load into a game.
#define GLASS_NODE_LIFETIME
#include "../ExperimentInstanceUpdates.cpp"

extern "C" __declspec(dllexport) void FixtureSourceSet(std::uint64_t proxy, unsigned action)
{
    std::lock_guard lock(sourceMutex);
    sourceReady = true;
    if (action == 1) sourceOwners.destroyed(0xabcdef, proxy);
    else sourceOwners.created(0xabcdef, proxy, {0x40000, 0x50000, 0x30000, 107, 40});
}
