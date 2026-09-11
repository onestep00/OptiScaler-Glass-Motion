#include "pch.h"
#include "GeometryViewRegistry.h"
#include "DetourThreads.h"
#include <hooks/Hook_Utils.h>

namespace GlassFg
{
namespace
{
using Device = ID3D12Device;
template <auto Method> using MethodType = typename rewrite_signature<decltype(Method)>::type;
MethodType<&Device::CreateDescriptorHeap> originalHeap = nullptr;
MethodType<&Device::CreateRenderTargetView> originalRtv = nullptr;
MethodType<&Device::CreateDepthStencilView> originalDsv = nullptr;
MethodType<&Device::CopyDescriptors> originalCopy = nullptr;
MethodType<&Device::CopyDescriptorsSimple> originalSimple = nullptr;
struct State { Device* device; GeometryViewRegistry registry; explicit State(Device* d) : device(d), registry(d) {} };
std::atomic<State*> published = nullptr;
std::mutex startup;
thread_local unsigned depth = 0;
struct Scope { bool outer = depth++ == 0; ~Scope() { --depth; } };
State* state(Device* device) { auto* value = published.load(std::memory_order_acquire); return value && value->device == device ? value : nullptr; }
void failed(Device* device) noexcept { if (auto* value = state(device)) value->registry.invalidate(); }
HRESULT WINAPI heap(Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID iid, void** output)
{
    Scope scope; const auto result = originalHeap(device, desc, iid, output);
    if (scope.outer && SUCCEEDED(result) && output && *output)
        try
        {
            if (auto* value = state(device))
            {
                Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> object;
                if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&object)))) value->registry.created(object.Get());
            }
        }
        catch (...) { failed(device); }
    return result;
}
void WINAPI rtv(Device* device, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    Scope scope; originalRtv(device, resource, desc, handle);
    if (scope.outer) try { if (auto* value = state(device)) value->registry.created(resource, desc, handle, 1); } catch (...) { failed(device); }
}
void WINAPI dsv(Device* device, ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    Scope scope; originalDsv(device, resource, desc, handle);
    if (scope.outer) try { if (auto* value = state(device)) value->registry.created(resource, desc, handle, 2); } catch (...) { failed(device); }
}
void WINAPI copy(Device* device, UINT dc, const D3D12_CPU_DESCRIPTOR_HANDLE* d, const UINT* ds,
                 UINT sc, const D3D12_CPU_DESCRIPTOR_HANDLE* s, const UINT* ss, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    Scope scope; originalCopy(device, dc, d, ds, sc, s, ss, type);
    if (scope.outer) try { if (auto* value = state(device)) value->registry.copy(dc, d, ds, sc, s, ss, type); } catch (...) { failed(device); }
}
void WINAPI simple(Device* device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE d, D3D12_CPU_DESCRIPTOR_HANDLE s, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    Scope scope; originalSimple(device, count, d, s, type);
    if (scope.outer) try { if (auto* value = state(device)) value->registry.copy(1, &d, &count, 1, &s, &count, type); } catch (...) { failed(device); }
}
template <class Function> bool attach(Function& original, void* address, Function replacement)
{
    original = reinterpret_cast<Function>(address);
    return address && DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(replacement)) == NO_ERROR;
}
}
bool StartGeometryViews(ID3D12Device* device) noexcept
{
    if (!device) return false;
    try
    {
        std::lock_guard lock(startup);
        if (auto* value = published.load()) return value->device == device;
        auto candidate = std::make_unique<State>(device);
        HMODULE resident = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&StartGeometryViews), &resident)) return false;
        DetourThreads threads;
        if (!threads.gather() || DetourTransactionBegin() != NO_ERROR) return false;
        auto** table = *reinterpret_cast<void***>(device);
        const bool okay = attach(originalHeap, table[14], heap) && attach(originalRtv, table[20], rtv) &&
                          attach(originalDsv, table[21], dsv) && attach(originalCopy, table[23], copy) &&
                          attach(originalSimple, table[24], simple);
        if (!okay || !threads.enlist()) { DetourTransactionAbort(); return false; }
        if (DetourTransactionCommit() != NO_ERROR) return false;
        published.store(candidate.release(), std::memory_order_release);
        return true;
    }
    catch (...) { return false; }
}
std::shared_ptr<const GeometryView> FindGeometryView(D3D12_CPU_DESCRIPTOR_HANDLE handle, uint32_t kind) noexcept
{
    try { if (auto* value = published.load(std::memory_order_acquire)) return value->registry.find(handle, kind); }
    catch (...) {}
    return {};
}
GeometryViewStats GetGeometryViewStats() noexcept
{
    if (auto* value = published.load(std::memory_order_acquire)) return value->registry.stats();
    return {};
}
} // namespace GlassFg
