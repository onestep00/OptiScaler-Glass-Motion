#pragma once
#include "ExperimentAbi.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace GlassFg
{
// Control-thread loader. The integration MUST retain each Frame until all its
// CPU callbacks, command recordings and GPU uses are finished. This class does
// not infer GPU completion from callback return or command-list Reset.
class ExperimentRuntime
{
    struct Module
    {
        HMODULE library = nullptr;
        GlassExperimentApi api {};
        void* context = nullptr;
        uint64_t generation = 0;
        std::filesystem::path path;
        ~Module()
        {
            if (context && api.destroy) api.destroy(context);
            if (library) FreeLibrary(library);
        }
    };
    std::array<std::shared_ptr<Module>, 3> slots;
    std::atomic<std::shared_ptr<Module>> active;
    std::atomic<uint64_t> activeCapabilities = 0;
    DWORD controlThread = GetCurrentThreadId();
    uint64_t generation = 0;
    void control() const
    {
        if (GetCurrentThreadId() != controlThread)
            throw std::runtime_error("Experiment lifecycle requires its control thread");
    }

  public:
    class Lease
    {
        friend class ExperimentRuntime;
        std::shared_ptr<Module> module;
        explicit Lease(std::shared_ptr<Module> value) : module(std::move(value)) {}
      public:
        Lease() = default;
        explicit operator bool() const { return bool(module); }
    };
    class Frame
    {
        friend class ExperimentRuntime;
        std::shared_ptr<Module> module;
        uint64_t frame = 0;
        uint32_t phases = 0, next = 1;
        Frame(std::shared_ptr<Module> value, uint64_t id, uint32_t count)
            : module(std::move(value)), frame(id), phases(count) {}
      public:
        Frame() = default;
        Frame(Frame&&) = default;
        Frame& operator=(Frame&&) = default;
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        explicit operator bool() const { return bool(module); }
        uint64_t revision() const { return module ? module->generation : 0; }
        bool supports(uint64_t capability) const
        { return module && (module->api.capabilities & capability) == capability; }
        Lease retain() const { return Lease(module); }
        int32_t dispatch(const GlassExperimentEvent& event)
        {
            if (!module || event.size != sizeof(event) || event.frame != frame ||
                !(module->api.capabilities & event.kind) ||
                (event.kind != GlassExperimentDraw && event.kind != GlassExperimentFg &&
                 event.kind != GlassExperimentCapture && event.kind != GlassExperimentSubmission) ||
                (event.payloadBytes && !event.payload))
                return -1;
            if (event.kind == GlassExperimentFg)
            {
                if (event.phaseCount != phases || event.phase != next || next > phases) return -1;
                ++next; // One attempt per phase, including an experiment error.
            }
            else if (event.phase || event.phaseCount) return -1;
            return module->api.event(module->context, &event);
        }
    };
    ExperimentRuntime() = default;
    ExperimentRuntime(const ExperimentRuntime&) = delete;
    ExperimentRuntime& operator=(const ExperimentRuntime&) = delete;
    ~ExperimentRuntime()
    {
        active.store({});
        // Host teardown without retirement proof never unloads referenced
        // executable code. At most three modules can be retained in this case.
        for (auto& slot : slots)
            if (slot && slot.use_count() != 1)
                (void)new std::shared_ptr<Module>(std::move(slot));
    }
    Frame beginFrame(uint64_t frame, uint32_t phases) const
    {
        if (!frame || !phases || phases > 3) return {};
        return Frame(active.load(), frame, phases);
    }
    bool censusEnabled() const noexcept
    { return (activeCapabilities.load(std::memory_order_acquire) & GlassExperimentCensus) != 0; }
    // CPU-only events need no invented engine/FG frame or GPU lease. The local
    // shared reference pins code through the callback; collection is on control.
    int32_t observe(const GlassExperimentEvent& event) const
    {
        if (event.size != sizeof(event) || event.kind != GlassExperimentCensus || event.phase || event.phaseCount ||
            (event.payloadBytes && !event.payload)) return -1;
        const auto module = active.load();
        if (!module || !(module->api.capabilities & GlassExperimentCensus)) return -1;
        return module->api.event(module->context, &event);
    }
    void disable()
    {
        control();
        activeCapabilities.store(0, std::memory_order_release);
        active.store({});
    }
    struct Status { uint64_t active = 0; unsigned loaded = 0; };
    Status status() const
    {
        control();
        const auto current = active.load();
        Status result { current ? current->generation : 0, 0 };
        for (const auto& slot : slots) if (slot) ++result.loaded;
        return result;
    }
    unsigned collect()
    {
        control();
        unsigned retired = 0;
        // An active module has a second reference in the atomic. A retired
        // module with any frame/GPU lease also cannot have a sole reference.
        for (auto& slot : slots)
            if (slot && slot.use_count() == 1) { slot.reset(); ++retired; }
        return retired;
    }
    uint64_t replace(const std::filesystem::path& input, const GlassExperimentHost& host)
    {
        control();
        if (!input.is_absolute() || host.size != sizeof(host) || host.abi != GLASS_EXPERIMENT_ABI)
            throw std::runtime_error("Invalid experiment path or host ABI");
        collect();
        const auto path = std::filesystem::canonical(input);
        std::shared_ptr<Module>* available = nullptr;
        for (auto& slot : slots)
        {
            if (slot && std::filesystem::equivalent(slot->path, path))
                throw std::runtime_error("Experiment file is still loaded; use a new build path");
            if (!slot) available = &slot;
        }
        if (!available) throw std::runtime_error("Experiment retirement capacity exhausted");
        auto candidate = std::make_shared<Module>();
        candidate->path = path;
        candidate->library = LoadLibraryExW(path.c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!candidate->library) throw std::runtime_error("Experiment DLL load failed");
        const auto query = reinterpret_cast<GlassExperimentQueryFn>(
            GetProcAddress(candidate->library, "GlassExperimentQuery"));
        const auto* api = query ? query() : nullptr;
        if (!api || api->size != sizeof(*api) || api->abi != GLASS_EXPERIMENT_ABI ||
            !api->create || !api->event || !api->destroy || !api->capabilities ||
            (api->capabilities & ~host.capabilities))
            throw std::runtime_error("Unsupported experiment ABI or capabilities");
        candidate->api = *api;
        if (api->create(&host, &candidate->context) != 0 || !candidate->context)
            throw std::runtime_error("Experiment preparation failed");
        candidate->generation = ++generation;
        *available = candidate;
        active.store(std::move(candidate));
        activeCapabilities.store(api->capabilities, std::memory_order_release);
        return generation;
    }
};
} // namespace GlassFg
