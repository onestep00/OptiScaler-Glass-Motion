#pragma once
#include "GlassRegionGpu.h"
#include "GlassControls.h"
#include "GlassGpuTimer.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace GlassFg
{
// The host supplies resource states and a surface snapshot copied on this same
// ordered command stream. Matching dimensions alone does not identify glass.
struct SurfaceSnapshot
{
    ID3D12Resource* resource = nullptr;
    uint64_t generation = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
};

struct Inputs
{
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    unsigned index = 0, count = 0, reset = 0;
    float scaleX = 0, scaleY = 0, jitterX = 0, jitterY = 0;
    std::array<float, 16> clipToPrevious {};

    bool valid() const
    {
        if (!motion || !color || !depth || (count != 1 && count != 3) || index < 1 || index > count ||
            !std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0 || scaleY <= 0 || !std::isfinite(jitterX) ||
            !std::isfinite(jitterY))
            return false;
        for (float number : clipToPrevious)
            if (!std::isfinite(number))
                return false;
        return true;
    }

    // Parameters can be NVSDK_NGX_Parameter or the replay's typed adapter. This
    // avoids assuming that MinGW and MSVC order overloaded virtual methods alike.
    template <class Parameters> static bool read(Parameters* params, Inputs& value)
    {
        if (!params)
            return false;
        void* matrix = nullptr;
        if (params->Get("DLSSG.MVecs", &value.motion) != 1 || !value.motion ||
            params->Get("DLSSG.HUDLess", &value.color) != 1 || !value.color ||
            params->Get("DLSSG.Depth", &value.depth) != 1 || !value.depth ||
            params->Get("DLSSG.MultiFrameIndex", &value.index) != 1 ||
            params->Get("DLSSG.MultiFrameCount", &value.count) != 1 || params->Get("DLSSG.Reset", &value.reset) != 1 ||
            params->Get("DLSSG.MvecScaleX", &value.scaleX) != 1 ||
            params->Get("DLSSG.MvecScaleY", &value.scaleY) != 1 ||
            params->Get("DLSSG.JitterOffsetX", &value.jitterX) != 1 ||
            params->Get("DLSSG.JitterOffsetY", &value.jitterY) != 1 ||
            params->Get("DLSSG.ClipToPrevClip", &matrix) != 1 || !matrix)
            return false;
        // Only the recorded 2x/4x conventions are supported by this candidate.
        if ((value.count != 1 && value.count != 3) || value.index < 1 || value.index > value.count ||
            !std::isfinite(value.scaleX) || !std::isfinite(value.scaleY) || value.scaleX <= 0 || value.scaleY <= 0 ||
            !std::isfinite(value.jitterX) || !std::isfinite(value.jitterY))
            return false;
        std::memcpy(value.clipToPrevious.data(), matrix, sizeof(value.clipToPrevious));
        for (float number : value.clipToPrevious)
            if (!std::isfinite(number))
                return false;
        return true;
    }

    bool sameRenderedFrame(const Inputs& other) const
    {
        return motion == other.motion && color == other.color && depth == other.depth && count == other.count &&
               reset == other.reset && scaleX == other.scaleX && scaleY == other.scaleY && jitterX == other.jitterX &&
               jitterY == other.jitterY && clipToPrevious == other.clipToPrevious;
    }
};

struct PreparedInputs
{
    ID3D12Resource* originalMotion = nullptr;
    ID3D12Resource* originalDepth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
};

// Keep this scope strictly around the native FG call. The shared parameter
// table is restored even when the provider returns an error or C++ unwinds.
template <class Parameters> class ScopedInputs
{
    Parameters* params = nullptr;
    PreparedInputs pair {};

  public:
    ScopedInputs(Parameters* parameters, const PreparedInputs& prepared)
    {
        ID3D12Resource* motion = nullptr;
        ID3D12Resource* depth = nullptr;
        if (!parameters || !prepared.originalMotion || !prepared.originalDepth || !prepared.motion || !prepared.depth ||
            parameters->Get("DLSSG.MVecs", &motion) != 1 || parameters->Get("DLSSG.Depth", &depth) != 1 ||
            motion != prepared.originalMotion || depth != prepared.originalDepth)
            return;
        params = parameters;
        pair = prepared;
        params->Set("DLSSG.MVecs", pair.motion);
        params->Set("DLSSG.Depth", pair.depth);
    }
    ScopedInputs(const ScopedInputs&) = delete;
    ScopedInputs& operator=(const ScopedInputs&) = delete;
    ~ScopedInputs()
    {
        if (params)
        {
            params->Set("DLSSG.MVecs", pair.originalMotion);
            params->Set("DLSSG.Depth", pair.originalDepth);
        }
    }
    bool applied() const { return params != nullptr; }
};

// One instance per native FG feature / ordered queue. The caller owns GPU
// completion, command-state restoration, and validated surface identification.
// No resource address, provider detour, input polling, or readback is used here.
class Pass
{
    GlassSurfaceGpu surface;
    GlassRegionGpu region;
    bool initialized = false;
    bool attempted = false;
    bool batchReady = false;
    uint64_t lastSurfaceGeneration = 0;
    unsigned nextIndex = 0;
    uint64_t dispatchCount = 0;
    Inputs batch {};
    ID3D12GraphicsCommandList* batchCommandList = nullptr;
    ID3D12Resource* batchSurface = nullptr;

    static bool texture2d(const D3D12_RESOURCE_DESC& desc)
    {
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width > 0 && desc.Height > 0 &&
               desc.Width <= 3840 && desc.Height <= 2160 && desc.MipLevels == 1 && desc.DepthOrArraySize == 1 &&
               desc.SampleDesc.Count == 1;
    }
    static bool depthFormat(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_R32_TYPELESS || format == DXGI_FORMAT_R32_FLOAT;
    }
    bool matches(const Inputs& inputs, const SurfaceSnapshot& snapshot) const
    {
        if (!inputs.motion || !inputs.color || !inputs.depth || !snapshot.resource || !snapshot.generation)
            return false;
        const auto mv = inputs.motion->GetDesc(), color = inputs.color->GetDesc();
        const auto depth = inputs.depth->GetDesc(), auxiliary = snapshot.resource->GetDesc();
        return texture2d(mv) && texture2d(color) && texture2d(depth) && texture2d(auxiliary) &&
               mv.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && depthFormat(depth.Format) &&
               depthFormat(auxiliary.Format) &&
               (color.Format == DXGI_FORMAT_R8G8B8A8_UNORM || color.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) &&
               mv.Width == surface.width && mv.Height == surface.height && color.Width == surface.colorWidth &&
               color.Height == surface.colorHeight && depth.Width == mv.Width && depth.Height == mv.Height &&
               auxiliary.Width == mv.Width && auxiliary.Height == mv.Height;
    }

  public:
    Pass() = default;
    Pass(const Pass&) = delete;
    Pass& operator=(const Pass&) = delete;

    bool initialize(ID3D12Device* device, const D3D12_RESOURCE_DESC* descriptions, const wchar_t* seedShader,
                    const wchar_t* regionShader, FILE* log)
    {
        if (attempted || !device || !descriptions || !seedShader || !regionShader || !log)
            return false;
        attempted = true;
        for (unsigned i = 0; i < 3; ++i)
            if (!texture2d(descriptions[i]))
                return false;
        if (descriptions[0].Format != DXGI_FORMAT_R16G16B16A16_FLOAT || !depthFormat(descriptions[2].Format) ||
            (descriptions[1].Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
             descriptions[1].Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) ||
            descriptions[2].Width != descriptions[0].Width || descriptions[2].Height != descriptions[0].Height)
            return false;
        initialized = surface.initialize(device, descriptions, seedShader, log) &&
                      region.initialize(device, surface, regionShader, log);
        if (!initialized)
            releaseAfterGpuDrain(); // Initialization has not submitted commands.
        return initialized;
    }

    void invalidateHistory()
    {
        batchReady = false;
        surface.history = false;
        nextIndex = 0;
    }

    // Input states come from the host; COPY_DEST is not inferred from a format.
    PreparedInputs prepare(ID3D12GraphicsCommandList* cmd, const Inputs& inputs, const SurfaceSnapshot& snapshot,
                           const D3D12_RESOURCE_STATES (&states)[3], Controls controls = { true, 100 },
                           GpuTimer* timer = nullptr)
    {
        if (inputs.index == 1 && !controls.active())
        {
            invalidateHistory();
            return {};
        }
        if (!initialized || !cmd || !inputs.valid() || !matches(inputs, snapshot) ||
            states[0] != D3D12_RESOURCE_STATE_COPY_DEST || states[2] != D3D12_RESOURCE_STATE_COPY_DEST ||
            (cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && cmd->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE))
        {
            invalidateHistory();
            return {};
        }
        if (inputs.index == 1)
        {
            batchReady = false;
            if (snapshot.generation <= lastSurfaceGeneration)
            {
                invalidateHistory();
                return {};
            }
            const auto timing =
                timer && controls.measureGpuTime && dispatchCount % 30 == 0 ? timer->begin(cmd) : GpuTimer::Ticket {};
            ID3D12Resource* originals[] = { inputs.motion, inputs.color, inputs.depth, snapshot.resource };
            for (unsigned i = 0; i < 4; ++i)
            {
                auto state = i < 3 ? states[i] : snapshot.state;
                if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
                    GlassSurfaceGpu::transition(cmd, originals[i], state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd->CopyResource(surface.inputs[i], originals[i]);
                if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
                    GlassSurfaceGpu::transition(cmd, originals[i], D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            }
            surface.dispatch(cmd, inputs.scaleX, inputs.scaleY, inputs.jitterX, inputs.jitterY,
                             inputs.clipToPrevious.data(), inputs.reset != 0, false, true, false);
            region.dispatch(cmd, surface, inputs.scaleX, inputs.scaleY, inputs.jitterX, inputs.jitterY,
                            inputs.clipToPrevious.data(), inputs.reset != 0, controls.coverage());
            if (timer && timing)
                timer->end(cmd, timing);
            ++dispatchCount;
            lastSurfaceGeneration = snapshot.generation;
            batch = inputs;
            batchCommandList = cmd;
            batchSurface = snapshot.resource;
            nextIndex = 2;
            batchReady = true;
        }
        else if (batchReady && inputs.index == nextIndex && inputs.index <= inputs.count && cmd == batchCommandList &&
                 snapshot.resource == batchSurface && snapshot.generation == lastSurfaceGeneration &&
                 inputs.sameRenderedFrame(batch))
        {
            ++nextIndex;
        }
        else
        {
            invalidateHistory();
            return {};
        }
        // Both outputs intentionally match the captured native FG COPY_DEST
        // contract. A host with a different input-state contract must adapt it.
        return { inputs.motion, inputs.depth, surface.output, surface.depthOutput };
    }

    uint64_t renderedDispatches() const { return dispatchCount; }
    ID3D12Resource* selection() const { return surface.selection; }
    ID3D12Resource* failures() { return region.failureBuffer(); }

    // Never call from DllMain or from an uncompleted command-list callback.
    void releaseAfterGpuDrain()
    {
        region.releaseAfterGpuDrain();
        surface.releaseAfterGpuDrain();
        initialized = false;
        invalidateHistory();
    }
};
} // namespace GlassFg
