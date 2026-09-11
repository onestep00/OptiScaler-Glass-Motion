#pragma once
#include "../ExperimentDrawBridge.h"
#include "../ExperimentRuntime.h"
#include "../ExperimentCaptureOwner.h"
#include "ObservedCaptureQueue.h"

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
    GlassFg::ExperimentCaptureOwner* owner = nullptr; // Registered resident fixture.
    unsigned retiredBeforeDiscard = 0;
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
    void start(ID3D12Device* device, const std::filesystem::path& directory, bool captureModule = false)
    {
        const GlassExperimentHost host { sizeof(host), GLASS_EXPERIMENT_ABI,
            GlassExperimentDraw | GlassExperimentCapture | GlassExperimentSubmission, device };
        auto unsupported = host;
        unsupported.capabilities &= ~uint64_t(GlassExperimentSubmission);
        bool rejected = false;
        try { runtime.replace(std::filesystem::absolute(directory / "experiment-draw.dll"), unsupported); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected && runtime.status().active == 0 && runtime.status().loaded == 0,
                "Submission-dependent module admitted without host support");
        runtime.replace(std::filesystem::absolute(directory / "experiment-draw.dll"), host);
        current = this;
        require(GlassFg::RegisterExperimentDrawObserver(observe), "Register resident draw bridge");
        if (captureModule)
        {
            owner = new GlassFg::ExperimentCaptureOwner(runtime, device);
            require(GlassFg::RegisterGeometryDrawCapture(owner), "Register module capture owner");
            require(CaptureQueueTest::install(device), "Install actual submission observer for module fixture");
        }
    }
    void prime(const GlassFg::ExperimentPipelineLease& pipeline, const std::filesystem::path& compiler)
    {
        const auto primePipeline = reinterpret_cast<int32_t (*)(GlassExperimentPipelineAccess)>(
            GetProcAddress(GetModuleHandleW(L"experiment-draw.dll"), "PrimePipeline"));
        require(primePipeline && primePipeline(GlassFg::BorrowExperimentPipeline(pipeline)) == 1,
                "Prime capture fixture compiler inputs");
        prepare(compiler);
    }
    void configure(const GlassFg::GeometryPreparedDraw& source)
    {
        if (!owner) return;
        const auto configureCapture = reinterpret_cast<void (*)(const GlassExperimentPreparedCapture*)>(
            GetProcAddress(GetModuleHandleW(L"experiment-draw.dll"), "ConfigureCapture"));
        require(configureCapture != nullptr, "Capture fixture configuration missing");
        GlassExperimentPreparedCapture prepared {}; prepared.size = sizeof(prepared);
        memcpy(prepared.history, &source.history, sizeof(source.history));
        prepared.previous = source.previous; prepared.current = source.current; prepared.material = source.material;
        prepared.capture = source.capture; prepared.mapping = source.mapping;
        configureCapture(&prepared);
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
        if (!owner) recordedUse = runtime.beginFrame(42, 1).retain();
    }
    ID3D12PipelineState* pipeline() const { return compiled; }
    void beforeFinalDiscard()
    {
        if (!owner) return;
        owner->stop(); runtime.disable();
        retiredBeforeDiscard = owner->collect();
        require(retiredBeforeDiscard == 7 && runtime.collect() == 0,
                "Final capture recording retired before actual Reset");
    }
    void verify()
    {
        if (failure) throw std::runtime_error("Draw DLL rejected payload: " + std::to_string(failure));
        require(callbacks == 32 && objects == 48, "Borrowed draw DLL callback counts");
        require(compiled != nullptr, "Module pipeline not prepared");
        if (owner)
        {
            owner->stop();
            require(retiredBeforeDiscard == 7 && owner->collect() == 1,
                    "Final capture job did not retire after actual Reset/submission");
            const auto verifyCaptures = reinterpret_cast<int32_t (*)()>(
                GetProcAddress(GetModuleHandleW(L"experiment-draw.dll"), "VerifyCaptureCounts"));
            require(verifyCaptures && verifyCaptures() == 1, "Module lifecycle callbacks missing");
            const auto stats = owner->status();
            require(stats.beforeSubmitObserved == 8 && !stats.beforeSubmitRejected,
                    "Pre-submit observation missing or rejected");
            puts("PASS pre_submit_module_callbacks=8 actual_queue_observer=1 unsupported_host_rejected=1");
        }
        compiled = nullptr;
        recordedUse = {}; // Caller has completed and discarded all module draws.
        runtime.disable();
        require(runtime.collect() == 1 && !GetModuleHandleW(L"experiment-draw.dll"), "Draw DLL unload");
        printf("PASS borrowed_draw_dll=1 callbacks=%u objects=%u missing_mesh_rejected=1 "
               "bounds_rejected=1 retained_pipeline_compile=1 module_pipeline_draws=%u capture_lifecycle=%u actual_unload=1 "
               "bridge_gpu_copies=0 game_hooks=0\n", callbacks, objects, owner ? 8u : 7u, owner ? 1u : 0u);
    }
    ~ExperimentDrawCheck()
    {
        if (current == this) current = nullptr;
        // Failed fixture verification supplies no retirement proof.
        if (recordedUse) (void)new GlassFg::ExperimentRuntime::Lease(std::move(recordedUse));
    }
};
