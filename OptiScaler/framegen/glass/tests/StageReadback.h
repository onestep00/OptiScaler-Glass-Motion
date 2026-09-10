#pragma once
// Diagnostic copies only. Caller supplies a live resource, its proven state,
// a recording outside render passes, and completion/discard ownership.
// The owner externally serializes calls; this is not a concurrent state tracker.
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace stage_capture
{
using Microsoft::WRL::ComPtr;
struct Region { UINT x{}, y{}, width{}, height{}; };
struct Layout
{
    D3D12_RESOURCE_DESC source{};
    Region region{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 rowBytes{}, totalBytes{};
};

inline UINT PixelBytes(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
    default: return 0;
    }
}

// No resizing or interpretation of color/exposure. This copies one color plane.
inline bool Describe(ID3D12Device* device, const D3D12_RESOURCE_DESC& source,
                     Region region, UINT64 budget, Layout& layout)
{
    const UINT pixelBytes = PixelBytes(source.Format);
    if (!device || !pixelBytes || source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        source.DepthOrArraySize != 1 || source.MipLevels != 1 || source.SampleDesc.Count != 1 ||
        source.Width > UINT_MAX || !source.Width || !source.Height || !region.width || !region.height ||
        region.x > source.Width || region.y > source.Height ||
        region.width > source.Width - region.x || region.height > source.Height - region.y)
        return false;
    auto cropped = source;
    cropped.Width = region.width;
    cropped.Height = region.height;
    cropped.Alignment = 0;
    cropped.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    cropped.Flags = D3D12_RESOURCE_FLAG_NONE;
    Layout result{};
    result.source = source;
    result.region = region;
    device->GetCopyableFootprints(&cropped, 0, 1, 0, &result.footprint,
                                 &result.rows, &result.rowBytes, &result.totalBytes);
    if (!result.totalBytes || result.totalBytes == UINT64_MAX || result.totalBytes > budget ||
        result.totalBytes > SIZE_MAX || result.rows != region.height ||
        result.rowBytes != UINT64(region.width) * pixelBytes ||
        result.footprint.Footprint.RowPitch < result.rowBytes)
        return false;
    layout = result;
    return true;
}

class Readback
{
    ComPtr<ID3D12Resource> source_, data_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12Fence> fence_;
    Layout layout_{};
    UINT64 completion_{};
    bool recorded_{}, completed_{}, discarded_{}, neverSubmitted_{}, uncertainSubmission_{};

public:
    Readback() = default;
    Readback(const Readback&) = delete;
    Readback& operator=(const Readback&) = delete;
    ~Readback()
    {
        // A closed command list may be submitted again. Completion alone never
        // permits release. A diagnostic that cannot prove discard intentionally
        // retains these two resources until process termination.
        if (recorded_ && !(discarded_ && !uncertainSubmission_ && (neverSubmitted_ || completed_)))
        {
            source_.Detach();
            data_.Detach();
            fence_.Detach();
        }
    }
    const Layout& layout() const { return layout_; }
    bool initialize(ID3D12Resource* liveSource, Region region, UINT64 budget)
    {
        if (!liveSource || source_ || recorded_ || discarded_) return false;
        ComPtr<ID3D12Device> device;
        if (FAILED(liveSource->GetDevice(IID_PPV_ARGS(&device)))) return false;
        Layout candidate{};
        if (!Describe(device.Get(), liveSource->GetDesc(), region, budget, candidate)) return false;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = candidate.totalBytes;
        desc.Height = desc.SampleDesc.Count = 1;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> data;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&data)))) return false;
        source_ = liveSource;
        device_ = device;
        data_ = data;
        layout_ = candidate;
        return true;
    }
    bool record(ID3D12GraphicsCommandList* command, D3D12_RESOURCE_STATES provenState)
    {
        if (!source_ || !command || recorded_ || discarded_) return false;
        // COMMON requires promotion/decay provenance. This diagnostic does not
        // invent it. Split transitions and an active render pass are rejected
        // by the caller before entry. COPY lists need separate queue ownership.
        const auto type = command->GetType();
        if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return false;
        if (provenState == D3D12_RESOURCE_STATE_COMMON) return false;
        if (type == D3D12_COMMAND_LIST_TYPE_COMPUTE &&
            (provenState & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE |
                           D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))) return false;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {source_.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              provenState, D3D12_RESOURCE_STATE_COPY_SOURCE};
        const bool transition = (provenState & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
        if (transition) command->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = source_.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = data_.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = layout_.footprint;
        const auto& r = layout_.region;
        D3D12_BOX box{r.x, r.y, 0, r.x+r.width, r.y+r.height, 1};
        command->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        if (transition)
        {
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            command->ResourceBarrier(1, &barrier);
        }
        recorded_ = true;
        return true;
    }
    // Host observes every submission. Bind once, after a successful queue Signal
    // following the actual submit. A repeated submission invalidates this copy.
    // A failed or uncertain Signal must not be reported as an unsubmitted copy.
    bool submitted(ID3D12Fence* fence, UINT64 value)
    {
        if (!recorded_ || neverSubmitted_ || fence_ || !fence || !value || value == UINT64_MAX)
        {
            uncertainSubmission_ = true;
            return false;
        }
        fence_ = fence;
        completion_ = value;
        return true;
    }
    // Host proves the recording cannot be resubmitted: successful Reset or
    // destruction. Discard alone does not imply completion of prior GPU use.
    void discarded() { discarded_ = true; }
    // Only for a Reset/destruction known to precede every queue submission.
    // An observed submission with a failed Signal does not satisfy this proof.
    bool discardedWithoutSubmission()
    {
        if (!recorded_ || completion_ || uncertainSubmission_) return false;
        discarded_ = neverSubmitted_ = true;
        return true;
    }
    bool read(std::vector<std::uint8_t>& packed)
    {
        // Both requirements prevent racing a later re-submission of the same
        // closed command recording after its first fence completed.
        if (!completion_ || !fence_ || !recorded_ || !discarded_ || uncertainSubmission_) return false;
        const auto done = fence_->GetCompletedValue();
        if (done == UINT64_MAX || done < completion_ || FAILED(device_->GetDeviceRemovedReason())) return false;
        completed_ = true;
        void* bytes = nullptr;
        D3D12_RANGE range{0, SIZE_T(layout_.totalBytes)};
        packed.resize(SIZE_T(layout_.rowBytes) * layout_.rows);
        if (FAILED(data_->Map(0, &range, &bytes))) return false;
        for (UINT y = 0; y < layout_.rows; ++y)
            std::memcpy(packed.data() + SIZE_T(y)*SIZE_T(layout_.rowBytes),
                        static_cast<const std::uint8_t*>(bytes) + layout_.footprint.Offset +
                        SIZE_T(y)*layout_.footprint.Footprint.RowPitch, SIZE_T(layout_.rowBytes));
        D3D12_RANGE unchanged{};
        data_->Unmap(0, &unchanged);
        return true;
    }
};
}
