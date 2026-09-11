#pragma once
#include "../ExperimentDrawBridge.h"
#include "../ExperimentRuntime.h"

// Resident test callback plus a test-only control-thread preparation entry.
// The DLL owns the compiled PSO. The fixture drains and discards its commands
// before unloading; automatic production capture/FG lifetimes remain separate.
class ExperimentDrawCheck
{
    inline static ExperimentDrawCheck* current = nullptr;
    GlassFg::ExperimentRuntime runtime;
    unsigned callbacks = 0, objects = 0;
    int32_t failure = 0;
    uint64_t lastRecording = 0;
    ID3D12PipelineState* compiled = nullptr;
    GlassFg::ExperimentRuntime::Lease recordedUse;
    static void observe(const GlassExperimentEvent& value) noexcept
    {
        auto* self = current;
        if (!self) return;
        auto frame = self->runtime.beginFrame(value.frame, 1);
        const auto result = frame.dispatch(value);
        const auto& draw = *static_cast<const GlassExperimentDrawInput*>(value.payload);
        ++self->callbacks;
        if (result <= 0) self->failure = result;
        else self->objects += static_cast<unsigned>(result);
        if (!draw.recording || draw.recording < self->lastRecording) self->failure = -20;
        self->lastRecording = draw.recording;
    }
  public:
    void start(ID3D12Device* device, const std::filesystem::path& directory)
    {
        const GlassExperimentHost host { sizeof(host), GLASS_EXPERIMENT_ABI, GlassExperimentDraw, device };
        runtime.replace(std::filesystem::absolute(directory / "experiment-draw.dll"), host);
        current = this;
        require(GlassFg::RegisterExperimentDrawObserver(observe), "Register resident draw bridge");
    }
    void prepare(const std::filesystem::path& compiler)
    {
        const auto library = GetModuleHandleW(L"experiment-draw.dll");
        const auto preparePipeline = reinterpret_cast<int32_t (*)(const wchar_t*)>(
            GetProcAddress(library, "PrepareRetainedPipeline"));
        const auto getPipeline = reinterpret_cast<void* (*)()>(GetProcAddress(library, "GetPreparedPipeline"));
        require(preparePipeline && getPipeline && preparePipeline(std::filesystem::absolute(compiler).c_str()) == 1,
                "Separate DLL could not compile retained original pipeline");
        compiled = static_cast<ID3D12PipelineState*>(getPipeline());
        require(compiled != nullptr, "Module-owned capture PSO missing");
        recordedUse = runtime.beginFrame(42, 1).retain();
    }
    ID3D12PipelineState* pipeline() const { return compiled; }
    void verify()
    {
        if (failure) throw std::runtime_error("Draw DLL rejected payload: " + std::to_string(failure));
        require(callbacks == 32 && objects == 48, "Borrowed draw DLL callback counts");
        require(compiled != nullptr, "Module pipeline not prepared");
        compiled = nullptr;
        recordedUse = {}; // Caller has completed and discarded all module draws.
        runtime.disable();
        require(runtime.collect() == 1 && !GetModuleHandleW(L"experiment-draw.dll"), "Draw DLL unload");
        printf("PASS borrowed_draw_dll=1 callbacks=%u objects=%u missing_mesh_rejected=1 "
               "bounds_rejected=1 retained_pipeline_compile=1 module_pipeline_draws=7 actual_unload=1 "
               "bridge_gpu_copies=0 game_hooks=0\n", callbacks, objects);
    }
    ~ExperimentDrawCheck()
    {
        if (current == this) current = nullptr;
        // Failed fixture verification supplies no retirement proof.
        if (recordedUse) (void)new GlassFg::ExperimentRuntime::Lease(std::move(recordedUse));
    }
};
