#include "../ExperimentRuntime.h"
#include <cstdio>
#include <thread>

void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void rejects(F call, const char* reason)
{
    bool rejected = false;
    try { call(); } catch (const std::exception&) { rejected = true; }
    require(rejected, reason);
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 2, "ExperimentRuntime <fresh fixture directory>");
        const auto dir = std::filesystem::canonical(argv[1]);
        const auto a = dir / "experiment-a.dll", b = dir / "experiment-b.dll";
        GlassFg::ExperimentRuntime runtime;
        GlassExperimentHost host { sizeof(host), GLASS_EXPERIMENT_ABI, 11, nullptr };
        const auto revisionA = runtime.replace(a, host);
        auto frameA = runtime.beginFrame(41, 3);
        GlassExperimentEvent call { sizeof(call), GlassExperimentFg, 41, 1, 3, 0, 0, nullptr };
        require(frameA.dispatch(call) == 1, "A phase one");
        const auto revisionB = runtime.replace(b, host);
        require(revisionB != revisionA, "Generation did not advance");
        for (unsigned phase = 2; phase <= 3; ++phase)
        {
            call.phase = phase;
            require(frameA.dispatch(call) == 1, "MFG frame mixed module generations");
        }
        require(frameA.dispatch(call) == -1, "Duplicate phase admitted");
        require(runtime.collect() == 0 && GetModuleHandleW(a.c_str()), "Referenced module unloaded");
        rejects([&] { runtime.replace(a, host); }, "Duplicate loaded path accepted");
        auto frameB = runtime.beginFrame(42, 1);
        call.frame = 42; call.phase = 1; call.phaseCount = 1;
        require(frameB.dispatch(call) == 2, "New frame did not switch");
        frameA = {};
        require(runtime.collect() == 1 && !GetModuleHandleW(a.c_str()), "A did not unload on control thread");
        rejects([&] { runtime.replace(dir / "experiment-fail.dll", host); }, "Failed create accepted");
        rejects([&] { runtime.replace(dir / "experiment-badabi.dll", host); }, "Wrong ABI accepted");
        require(runtime.beginFrame(43, 1).revision() == revisionB, "Preparation failure changed active module");
        runtime.replace(a, host);
        auto blocked = runtime.beginFrame(44, 1);
        HANDLE events[] { CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr) };
        require(events[0] && events[1], "Test event creation failed");
        GlassExperimentEvent blocking { sizeof(blocking), GlassExperimentDraw, 44, 0, 0, 99, sizeof(events), events };
        std::thread callback([&] { require(blocked.dispatch(blocking) == 1, "Blocked callback changed"); });
        require(WaitForSingleObject(events[0], 10000) == WAIT_OBJECT_0, "Callback did not enter");
        runtime.disable();
        require(!runtime.beginFrame(45, 1), "Disabled runtime admitted new frame");
        runtime.collect();
        require(GetModuleHandleW(a.c_str()) != nullptr, "Executing module unloaded");
        SetEvent(events[1]); callback.join();
        CloseHandle(events[0]); CloseHandle(events[1]);
        blocked = {}; frameB = {};
        require(runtime.collect() == 2, "Stopped modules not retired");
        require(!GetModuleHandleW(a.c_str()) && !GetModuleHandleW(b.c_str()), "Module residue after completed CPU use");
        runtime.replace(a, host);
        require(runtime.censusEnabled(), "Active census capability missing");
        HANDLE censusEvents[] { CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr) };
        require(censusEvents[0] && censusEvents[1], "Census test events");
        GlassExperimentEvent census { sizeof(census), GlassExperimentCensus, 0, 0, 0, 99, sizeof(censusEvents), censusEvents };
        std::thread observer([&] { require(runtime.observe(census) == 1, "Executing census generation changed"); });
        require(WaitForSingleObject(censusEvents[0], 10000) == WAIT_OBJECT_0, "Census did not enter");
        runtime.replace(b, host);
        require(runtime.collect() == 0 && GetModuleHandleW(a.c_str()), "Executing census DLL unloaded");
        runtime.disable(); require(!runtime.censusEnabled() && runtime.observe(census) == -1, "Disabled census admitted");
        require(runtime.collect() == 1 && GetModuleHandleW(a.c_str()), "Disabled executing census released");
        SetEvent(censusEvents[1]); observer.join();
        CloseHandle(censusEvents[0]); CloseHandle(censusEvents[1]);
        require(runtime.collect() == 1 && !GetModuleHandleW(a.c_str()), "Completed census not released");
        puts("PASS dynamic_dll_a_b_a=1 phase_generation_fixed=1 failed_prepare_rollback=1 wrong_abi_rejected=1 "
             "inflight_cpu_retained=1 unframed_census_retained=1 unloaded_after_release=1 game_hooks=0 gpu_completion_tested=0");
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
