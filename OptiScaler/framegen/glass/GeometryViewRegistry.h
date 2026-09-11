#pragma once
#include "GeometryViews.h"
#include "CommandLifetime.h"
#include <wrl/client.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GlassFg
{
class GeometryViewRegistry
{
    static constexpr size_t HeapLimit = 2048, ViewLimit = 131072;
    struct Heap
    {
        CommandLifetime lifetime;
        uint64_t identity = 0;
        SIZE_T base = 0;
        UINT count = 0, increment = 0;
        D3D12_DESCRIPTOR_HEAP_TYPE type {};
    };
    struct Entry { std::shared_ptr<Heap> heap; std::shared_ptr<const GeometryView> view; };
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::mutex mutex;
    std::vector<std::shared_ptr<Heap>> heaps;
    std::unordered_map<SIZE_T, Entry> views;
    uint64_t nextHeap = 0, nextView = 0;
    std::atomic<bool> healthy = true;
    std::atomic<uint64_t> heapCount = 0, writes = 0, copies = 0, lookups = 0, misses = 0;
    inline static std::atomic<uint64_t> nextResource = 0;
    inline static constexpr GUID ResourceKey { 0x8fc3d0c2, 0x7b2a, 0x47c7,
                                               { 0x85, 0x6c, 0xef, 0x16, 0x2d, 0x4d, 0x28, 0x6e } };
    static bool live(const std::shared_ptr<Heap>& heap)
    { return heap && !heap->lifetime.wasDestroyed(); }
    void compact()
    {
        std::erase_if(views, [](const auto& entry) { return !live(entry.second.heap); });
        std::erase_if(heaps, [](const auto& heap) { return !live(heap); });
    }
    std::shared_ptr<Heap> containing(SIZE_T handle, D3D12_DESCRIPTOR_HEAP_TYPE type)
    {
        for (const auto& heap : heaps)
            if (live(heap) && heap->type == type && handle >= heap->base &&
                (handle - heap->base) % heap->increment == 0 &&
                (handle - heap->base) / heap->increment < heap->count) return heap;
        return {};
    }
    uint64_t resourceIdentity(ID3D12Resource* resource)
    {
        uint64_t value = 0; UINT bytes = sizeof(value);
        if (SUCCEEDED(resource->GetPrivateData(ResourceKey, &bytes, &value)))
            return bytes == sizeof(value) ? value : 0;
        value = ++nextResource;
        if (!value || FAILED(resource->SetPrivateData(ResourceKey, sizeof(value), &value))) return 0;
        return value;
    }
    std::shared_ptr<const GeometryView> findLocked(SIZE_T handle, uint32_t kind)
    {
        const auto found = views.find(handle);
        return found != views.end() && live(found->second.heap) && found->second.view->kind == kind
                   ? found->second.view : nullptr;
    }
    void store(SIZE_T handle, const std::shared_ptr<Heap>& heap, std::shared_ptr<GeometryView> value)
    {
        if (!views.contains(handle) && views.size() == ViewLimit) compact();
        if ((!views.contains(handle) && views.size() == ViewLimit) || nextView == UINT64_MAX)
        { views.erase(handle); return; }
        value->handle = handle; value->heap = heap->identity; value->revision = ++nextView;
        views[handle] = { heap, std::move(value) };
    }
  public:
    void invalidate() noexcept { healthy.store(false, std::memory_order_release); }
    GeometryViewStats stats() const noexcept
    { return { true, healthy.load(), heapCount.load(), writes.load(), copies.load(), lookups.load(), misses.load() }; }
    explicit GeometryViewRegistry(ID3D12Device* d) : device(d)
    { heaps.reserve(HeapLimit); views.reserve(ViewLimit); }
    void created(ID3D12DescriptorHeap* object)
    {
        if (!object || !healthy.load(std::memory_order_acquire)) return;
        const auto desc = object->GetDesc();
        if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) return;
        std::lock_guard lock(mutex);
        std::erase_if(heaps, [](const auto& heap) { return !live(heap); });
        const auto base = object->GetCPUDescriptorHandleForHeapStart().ptr;
        const auto increment = device->GetDescriptorHandleIncrementSize(desc.Type);
        if (!base || !increment || !desc.NumDescriptors || heaps.size() == HeapLimit || nextHeap == UINT64_MAX ||
            uint64_t(desc.NumDescriptors) * increment > SIZE_MAX - base) return;
        for (const auto& existing : heaps)
            if (existing->lifetime.identity() == object && live(existing)) return;
        auto heap = std::make_shared<Heap>();
        if (!heap->lifetime.attach(object)) return;
        heap->identity = ++nextHeap; heap->base = base; heap->increment = increment;
        heap->count = desc.NumDescriptors; heap->type = desc.Type;
        heaps.push_back(std::move(heap));
        ++heapCount;
    }
    template <class Descriptor> void created(ID3D12Resource* resource, const Descriptor* desc,
                                             D3D12_CPU_DESCRIPTOR_HANDLE handle, uint32_t kind)
    {
        if (!healthy.load(std::memory_order_acquire)) return;
        ++writes;
        std::lock_guard lock(mutex);
        views.erase(handle.ptr); // Unknown/new writes cannot preserve an older descriptor.
        const auto type = kind == 1 ? D3D12_DESCRIPTOR_HEAP_TYPE_RTV : D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        auto heap = containing(handle.ptr, type);
        if (!heap || (!resource && !desc)) return;
        auto view = std::make_shared<GeometryView>();
        view->kind = kind; view->defaultDescriptor = !desc; view->nullResource = !resource;
        if (resource)
        {
            view->resource = resourceIdentity(resource);
            if (!view->resource) return;
            view->address = reinterpret_cast<uint64_t>(resource); view->allocation = resource->GetDesc();
        }
        if constexpr (std::is_same_v<Descriptor, D3D12_RENDER_TARGET_VIEW_DESC>)
        { if (desc) view->rtv = *desc; }
        else { if (desc) view->dsv = *desc; }
        store(handle.ptr, heap, std::move(view));
    }
    void copy(UINT destinationCount, const D3D12_CPU_DESCRIPTOR_HANDLE* destinations, const UINT* destinationSizes,
              UINT sourceCount, const D3D12_CPU_DESCRIPTOR_HANDLE* sources, const UINT* sourceSizes,
              D3D12_DESCRIPTOR_HEAP_TYPE type)
    {
        if (!healthy.load(std::memory_order_acquire) ||
            (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV)) return;
        ++copies;
        std::lock_guard lock(mutex);
        if (destinationCount > 4096 || sourceCount > 4096 || (destinationCount && !destinations) || (sourceCount && !sources))
        { views.clear(); return; }
        uint64_t destinationsTotal = 0, sourcesTotal = 0;
        for (UINT i = 0; i < destinationCount; ++i) destinationsTotal += destinationSizes ? destinationSizes[i] : 1;
        for (UINT i = 0; i < sourceCount; ++i) sourcesTotal += sourceSizes ? sourceSizes[i] : 1;
        if (destinationsTotal != sourcesTotal || destinationsTotal > 65536) { views.clear(); return; }
        const UINT increment = device->GetDescriptorHandleIncrementSize(type), kind = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? 1 : 2;
        if (!increment) { views.clear(); return; }
        // Legal D3D12 source and destination descriptor ranges do not overlap.
        UINT sourceRange = 0, sourceOffset = 0;
        for (UINT range = 0; range < destinationCount; ++range)
            for (UINT offset = 0, count = destinationSizes ? destinationSizes[range] : 1; offset < count; ++offset)
            {
                while (sourceRange < sourceCount && sourceOffset == (sourceSizes ? sourceSizes[sourceRange] : 1))
                { ++sourceRange; sourceOffset = 0; }
                if (sourceRange == sourceCount || SIZE_T(offset) * increment > SIZE_MAX - destinations[range].ptr ||
                    SIZE_T(sourceOffset) * increment > SIZE_MAX - sources[sourceRange].ptr) { views.clear(); return; }
                const auto destination = destinations[range].ptr + SIZE_T(offset) * increment;
                const auto source = sources[sourceRange].ptr + SIZE_T(sourceOffset++) * increment;
                const auto original = findLocked(source, kind);
                const auto heap = containing(destination, type);
                views.erase(destination);
                if (original && heap) store(destination, heap, std::make_shared<GeometryView>(*original));
            }
    }
    std::shared_ptr<const GeometryView> find(D3D12_CPU_DESCRIPTOR_HANDLE handle, uint32_t kind)
    {
        if (!healthy.load(std::memory_order_acquire)) return {};
        ++lookups;
        std::unique_lock lock(mutex, std::try_to_lock);
        auto result = lock ? findLocked(handle.ptr, kind) : nullptr;
        if (!result) ++misses;
        return result;
    }
};
} // namespace GlassFg
