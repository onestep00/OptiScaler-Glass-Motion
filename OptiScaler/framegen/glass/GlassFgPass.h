#pragma once
#include "GlassRegionGpu.h"
#include "GlassControls.h"
#include "GlassGpuTimer.h"
#include "GlassTrace.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

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
    // Frame depth convention the provider declares (DLSSG.DepthInverted).
    // Cyberpunk declares 1 (reverse-Z); absent key keeps that default.
    unsigned depthInverted = 1;
    float scaleX = 0, scaleY = 0, jitterX = 0, jitterY = 0;
    std::array<float, 16> clipToPrevious {};
    // Parameter key names the two textures were read from. The game-level block
    // uses the DLSSG.* names; the block Streamline's frame generation plugin
    // hands to the NGX core (via _nvngx.dll) uses MotionVectors/Depth.
    const char* motionKey = "DLSSG.MVecs";
    const char* depthKey = "DLSSG.Depth";
    // True only when the evaluation carried the DLSS-G parameter identity
    // (DLSSG.MVecs plus the multi-frame metadata). The frame generator is the
    // only consumer that names those keys, so this is also the gate that keeps
    // the correction off the upscaler and Ray Reconstruction, which share the
    // MotionVectors/Depth names on their own evaluations.
    bool frameGeneration = true;
    // Native Streamline frame domain, independent of the engine render tick.
    // UINT64_MAX means absent (for example an older replay); never infer it
    // from a repeated resource address or the multipass interpolation index.
    std::uint64_t frame = UINT64_MAX;
    // Which DLSSG.* keys the table carried. Zero on the upscaler and Ray
    // Reconstruction tables, so a non-zero mask is the frame generation
    // identity even when the textures are named MotionVectors/Depth.
    unsigned identityMask = 0;

    // The keys only a frame generation evaluation publishes. The upscaler and
    // Ray Reconstruction never set any of them, so one present marker is enough
    // to tell the generator from the other two features that share the
    // MotionVectors/Depth texture names.
    static constexpr unsigned markerCount = 12;
    static const char* const* markers() noexcept
    {
        static const char* const names[markerCount] {
            "DLSSG.MVecs",       "DLSSG.HUDLess",        "DLSSG.Depth",          "DLSSG.MultiFrameIndex",
            "DLSSG.MultiFrameCount", "DLSSG.Reset",      "DLSSG.ClipToPrevClip", "DLSSG.MvecScaleX",
            "DLSSG.MvecScaleY",  "DLSSG.OpticalFlowEnabled", "DLSSG.CameraNear", "DLSSG.CameraFar",
        };
        return names;
    }
    template <class Parameters> static unsigned markerMask(Parameters* params)
    {
        if (!params)
            return 0;
        const char* const* names = markers();
        unsigned mask = 0;
        for (unsigned i = 0; i < markerCount; ++i)
        {
            void* value = nullptr;
            if (params->Get(names[i], &value) == 1)
                mask |= (1u << i);
        }
        return mask;
    }
    // True when the table names at least one DLSS-G only parameter. The texture
    // aliases alone cannot prove the identity; these keys can.
    static bool tableNamesFrameGeneration(unsigned mask) noexcept
    {
        static constexpr unsigned strong = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 9);
        return (mask & strong) != 0;
    }

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
        if (params->Get(value.motionKey, &value.motion) != 1 || !value.motion ||
            params->Get("DLSSG.HUDLess", &value.color) != 1 || !value.color ||
            params->Get(value.depthKey, &value.depth) != 1 || !value.depth ||
            params->Get("DLSSG.MultiFrameIndex", &value.index) != 1 ||
            (params->Get("DLSSG.DepthInverted", &value.depthInverted), false) ||
            params->Get("DLSSG.MultiFrameCount", &value.count) != 1 || params->Get("DLSSG.Reset", &value.reset) != 1 ||
            params->Get("DLSSG.MvecScaleX", &value.scaleX) != 1 ||
            params->Get("DLSSG.MvecScaleY", &value.scaleY) != 1 ||
            params->Get("DLSSG.JitterOffsetX", &value.jitterX) != 1 ||
            params->Get("DLSSG.JitterOffsetY", &value.jitterY) != 1 ||
            params->Get("DLSSG.ClipToPrevClip", &matrix) != 1 || !matrix)
        {
            // Driver-level block: only the two textures are named. The caller
            // fills the missing motion-vector metadata from the Streamline
            // constants it already tracks, so the compose sees the same units.
            value.motionKey = "MotionVectors";
            value.depthKey = "Depth";
            // The shared names are not a frame generation identity on their
            // own: the upscaler and Ray Reconstruction read exactly the same
            // two keys. The DLSSG.* parameters in the same table are, because
            // only the generator publishes them. Streamline's plugin hands its
            // evaluation in with the generic texture names and the DLSS-G
            // metadata, which is the shape this branch has to admit.
            value.identityMask = markerMask(params);
            value.frameGeneration = tableNamesFrameGeneration(value.identityMask);
            if (params->Get(value.motionKey, &value.motion) != 1 || !value.motion ||
                params->Get(value.depthKey, &value.depth) != 1 || !value.depth)
                return false;
            value.color = value.motion;
            value.index = 1;
            value.count = 1;
            value.reset = 0;
            value.scaleX = 1.f;
            value.scaleY = 1.f;
            value.jitterX = 0.f;
            value.jitterY = 0.f;
            value.clipToPrevious.fill(0.f);
            for (unsigned i = 0; i < 4; ++i)
                value.clipToPrevious[i * 4 + i] = 1.f;
            return true;
        }
        // Only the recorded 2x/4x conventions are supported by this candidate.
        if ((value.count != 1 && value.count != 3) || value.index < 1 || value.index > value.count ||
            !std::isfinite(value.scaleX) || !std::isfinite(value.scaleY) || value.scaleX <= 0 || value.scaleY <= 0 ||
            !std::isfinite(value.jitterX) || !std::isfinite(value.jitterY))
            return false;
        std::memcpy(value.clipToPrevious.data(), matrix, sizeof(value.clipToPrevious));
        for (float number : value.clipToPrevious)
            if (!std::isfinite(number))
                return false;
        value.frameGeneration = true;
        value.identityMask = markerMask(params);
        return true;
    }

    bool sameRenderedFrame(const Inputs& other) const
    {
        return motion == other.motion && color == other.color && depth == other.depth && count == other.count &&
               reset == other.reset && scaleX == other.scaleX && scaleY == other.scaleY && jitterX == other.jitterX &&
               jitterY == other.jitterY && clipToPrevious == other.clipToPrevious && frame == other.frame;
    }
};

struct PreparedInputs
{
    ID3D12Resource* originalMotion = nullptr;
    ID3D12Resource* originalDepth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
    const char* motionKey = "DLSSG.MVecs";
    const char* depthKey = "DLSSG.Depth";
    // The same textures are published under the provider's own names too (the
    // parameter trace records MotionVectors/Depth reads). Whichever name the
    // consumer reads during the evaluation has to see the corrected texture, so
    // both are swapped when the table holds them.
    const char* motionAlias = "MotionVectors";
    const char* depthAlias = "Depth";
    // Documented DLSS-G transparency-layer inputs. The provider asks for these
    // keys on every evaluation (the parameter trace records
    // DLSS.TransparencyLayerMvecs and DLSS.TransparencyLayerOpacity with null
    // pointers) and the game leaves them empty. Answering them with the packed
    // coverage and the composed motion declares the transparent surface as its
    // own layer, so the content behind it stops carrying the region.
    ID3D12Resource* layerMvecs = nullptr;
    ID3D12Resource* layerOpacity = nullptr;
    const char* layerMvecsKey = "DLSS.TransparencyLayerMvecs";
    const char* layerOpacityKey = "DLSS.TransparencyLayerOpacity";
    // Graft-variant draws of the packed capture frame these inputs substitute
    // (PackedMotionFrame::graftDraws). Zero for every other producer.
    std::uint64_t graftDraws = 0;

    // Positional aggregate initialization broke twice already (a new field in
    // the middle silently re-bound the resource pointers to the key strings).
    // The two producers fill exactly these eight fields.
    static PreparedInputs make(ID3D12Resource* originalMotion, ID3D12Resource* originalDepth,
                               ID3D12Resource* motion, ID3D12Resource* depth, const char* motionKey,
                               const char* depthKey, ID3D12Resource* layerMvecs,
                               ID3D12Resource* layerOpacity) noexcept
    {
        PreparedInputs value;
        value.originalMotion = originalMotion;
        value.originalDepth = originalDepth;
        value.motion = motion;
        value.depth = depth;
        value.motionKey = motionKey;
        value.depthKey = depthKey;
        value.layerMvecs = layerMvecs;
        value.layerOpacity = layerOpacity;
        return value;
    }
};

// Keep this scope strictly around the native FG call. The shared parameter
// table is restored even when the provider returns an error or C++ unwinds.
template <class Parameters> class ScopedInputs
{
    Parameters* params = nullptr;
    PreparedInputs pair {};
    // Names that were actually swapped, so the restore is exact and a name the
    // table does not publish is never created.
    const char* motionNames[2] { nullptr, nullptr };
    const char* depthNames[2] { nullptr, nullptr };
    unsigned motionCount = 0, depthCount = 0;

    static bool same(const char* left, const char* right) noexcept
    {
        return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
    }
    static bool holds(Parameters* parameters, const char* name, ID3D12Resource* original) noexcept
    {
        ID3D12Resource* current = nullptr;
        return parameters != nullptr && name != nullptr && original != nullptr &&
               parameters->Get(name, &current) == 1 && current == original;
    }
    void swap(Parameters* parameters, const char* name, ID3D12Resource* original, ID3D12Resource* replacement,
              const char* (&names)[2], unsigned& count) noexcept
    {
        if (name == nullptr || original == nullptr || replacement == nullptr)
            return;
        for (unsigned i = 0; i < count; ++i)
            if (same(names[i], name))
                return;
        if (!holds(parameters, name, original))
            return;
        parameters->Set(name, replacement);
        names[count++] = name;
    }

  public:
    ScopedInputs(Parameters* parameters, const PreparedInputs& prepared)
    {
        if (!parameters || !prepared.originalMotion || !prepared.originalDepth || !prepared.motion || !prepared.depth)
            return;
        swap(parameters, prepared.motionKey, prepared.originalMotion, prepared.motion, motionNames, motionCount);
        swap(parameters, prepared.depthKey, prepared.originalDepth, prepared.depth, depthNames, depthCount);
        swap(parameters, prepared.motionAlias, prepared.originalMotion, prepared.motion, motionNames, motionCount);
        swap(parameters, prepared.depthAlias, prepared.originalDepth, prepared.depth, depthNames, depthCount);
        if (motionCount == 0 && depthCount == 0)
            return;
        params = parameters;
        pair = prepared;
    }
    ScopedInputs(const ScopedInputs&) = delete;
    ScopedInputs& operator=(const ScopedInputs&) = delete;
    ~ScopedInputs()
    {
        if (!params)
            return;
        for (unsigned i = 0; i < motionCount; ++i)
            params->Set(motionNames[i], pair.originalMotion);
        for (unsigned i = 0; i < depthCount; ++i)
            params->Set(depthNames[i], pair.originalDepth);
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
            // The region layer is an own-layer supply, not a blend strength: the
            // delivered motion either replaces the engine value inside the mask
            // or stays out of it, so the layer is built at full weight.
            region.dispatch(cmd, surface, inputs.scaleX, inputs.scaleY, inputs.jitterX, inputs.jitterY,
                            inputs.clipToPrevious.data(), inputs.reset != 0, 1.f);
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
