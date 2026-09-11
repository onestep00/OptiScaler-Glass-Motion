// Independent executable only. Exercises the linked Detours library, not game hooks.
#include "../DetourThreads.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using Function = unsigned char (*)(void*, float, void*, void*);
std::atomic<unsigned> originals = 0, oldCalls = 0, newCalls = 0, errors = 0;
int a = 1, b = 2, c = 3;
__declspec(noinline) unsigned char target(void* first, float value, void* second, void* third)
{
    if (first != &a || value != 3.25f || second != &b || third != &c) ++errors;
    ++originals;
    return 197;
}
Function older = target, newer = target;
unsigned char oldHook(void* first, float value, void* second, void* third)
{
    ++oldCalls;
    return older(first, value, second, third);
}
unsigned char newHook(void* first, float value, void* second, void* third)
{
    ++newCalls;
    return newer(first, value, second, third);
}
void require(bool value) { if (!value) throw std::runtime_error("Detour chain test failed"); }
void change(Function& original, Function replacement, bool attach)
{
    GlassFg::DetourThreads threads;
    require(threads.gather() && DetourTransactionBegin() == NO_ERROR);
    const auto result = attach ? DetourAttach(reinterpret_cast<PVOID*>(&original), replacement)
                               : DetourDetach(reinterpret_cast<PVOID*>(&original), replacement);
    if (result != NO_ERROR || !threads.enlist())
    {
        DetourTransactionAbort(); require(false);
    }
    require(DetourTransactionCommit() == NO_ERROR);
}
int main()
{
    try
    {
        Function volatile invoke = target;
        require(invoke(&a, 3.25f, &b, &c) == 197 && originals == 1);
        change(older, oldHook, true);
        std::array<unsigned char, 16> firstPatch {};
        memcpy(firstPatch.data(), reinterpret_cast<const void*>(target), firstPatch.size());
        require(invoke(&a, 3.25f, &b, &c) == 197 && originals == 2 && oldCalls == 1);
        change(newer, newHook, true);
        require(invoke(&a, 3.25f, &b, &c) == 197 && originals == 3 && oldCalls == 2 && newCalls == 1);
        change(newer, newHook, false);
        require(!memcmp(firstPatch.data(), reinterpret_cast<const void*>(target), firstPatch.size()));
        require(invoke(&a, 3.25f, &b, &c) == 197 && originals == 4 && oldCalls == 3 && newCalls == 1);
        std::atomic<bool> run = true;
        std::atomic<unsigned> invoked = 0;
        std::vector<std::thread> workers;
        for (unsigned i = 0; i < 4; ++i)
            workers.emplace_back([&] {
                while (run.load())
                {
                    if (invoke(&a, 3.25f, &b, &c) != 197) ++errors;
                    ++invoked;
                }
            });
        for (unsigned i = 0; i < 10; ++i)
        {
            newer = target;
            change(newer, newHook, true);
            change(newer, newHook, false);
        }
        run = false;
        for (auto& worker : workers) worker.join();
        require(!errors && originals == invoked + 4 && oldCalls == invoked + 3);
        require(!memcmp(firstPatch.data(), reinterpret_cast<const void*>(target), firstPatch.size()));
        change(older, oldHook, false);
        std::cout << "PASS chained_callbacks_once=1 mixed_arguments=1 return_preserved=1 "
                     "old_patch_restored=1 concurrent_replacements=10 invocations=" << invoked
                  << " game_hooks_installed=0\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n'; return 1;
    }
}
