#pragma once
#include "../ExperimentDrawBridge.h"
#include "../ExperimentRuntime.h"

// Resident test callback: the replaceable DLL only observes borrowed input.
// No GPU work is delegated and no engine-to-FG frame correlation is claimed.
class ExperimentDrawCheck
{
    inline static ExperimentDrawCheck* current = nullptr;
    GlassFg::ExperimentRuntime runtime;
    unsigned callbacks = 0, objects = 0;
    int32_t failure = 0;
    uint64_t lastRecording = 0;
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
    void verify()
    {
        if (failure) throw std::runtime_error("Draw DLL rejected payload: " + std::to_string(failure));
        require(callbacks == 32 && objects == 48, "Borrowed draw DLL callback counts");
        runtime.disable();
        require(runtime.collect() == 1 && !GetModuleHandleW(L"experiment-draw.dll"), "Draw DLL unload");
        printf("PASS borrowed_draw_dll=1 callbacks=%u objects=%u missing_mesh_rejected=1 "
               "bounds_rejected=1 actual_unload=1 bridge_gpu_copies=0 game_hooks=0\n", callbacks, objects);
    }
    ~ExperimentDrawCheck() { if (current == this) current = nullptr; }
};
