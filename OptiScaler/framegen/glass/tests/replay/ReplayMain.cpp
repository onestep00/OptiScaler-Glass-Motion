// Standalone initialization audit. No game attachment, window or user input.
// The filename includes nvngx.dll, as required by the snippet's caller check.
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <nvsdk_ngx_defs.h>
struct Value
{
    unsigned long long bits {};
    unsigned kind {};
};
struct Params
{
    void** table;
    std::map<std::string, Value> values;
};
static const char* phase = "initialization";
static unsigned replayGenerated = 1;
static nlohmann::json replayConfig;
static unsigned outputWidth, outputHeight, renderWidth, renderHeight, replayFrames, sdkVersion;
static unsigned long long applicationId;
template <class T, unsigned Kind> static void set(Params* p, const char* key, T value)
{
    Value v {};
    v.kind = Kind;
    memcpy(&v.bits, &value, sizeof(value));
    p->values[key] = v;
    printf("SET phase=%s kind=%u key=%s bits=%016llx\n", phase, Kind, key, v.bits);
}
template <class T, unsigned Kind> static unsigned get(Params* p, const char* key, T* out)
{
    auto it = p->values.find(key);
    unsigned result = 0xbad00010;
    if (it != p->values.end() && out)
    {
        const auto& v = it->second;
        if (v.kind == Kind || (v.kind <= 2 && Kind <= 2))
        {
            memcpy(out, &v.bits, sizeof(T));
            result = 1;
        }
        else if constexpr (std::is_integral_v<T>)
        {
            if (v.kind == 3 || v.kind == 4 || v.kind == 7)
            {
                *out = (T) v.bits;
                result = 1;
            }
        }
    }
    printf("GET phase=%s kind=%u key=%s result=%08x\n", phase, Kind, key, result);
    return result;
}
static void reset(Params* p) { p->values.clear(); }
static void* table[] = { (void*) set<void*, 0>,
                         (void*) set<void*, 1>,
                         (void*) set<void*, 2>,
                         (void*) set<int, 3>,
                         (void*) set<unsigned, 4>,
                         (void*) set<double, 5>,
                         (void*) set<float, 6>,
                         (void*) set<unsigned long long, 7>,
                         (void*) get<void*, 0>,
                         (void*) get<void*, 1>,
                         (void*) get<void*, 2>,
                         (void*) get<int, 3>,
                         (void*) get<unsigned, 4>,
                         (void*) get<double, 5>,
                         (void*) get<float, 6>,
                         (void*) get<unsigned long long, 7>,
                         (void*) reset };
static bool check(HRESULT hr, const char* name)
{
    printf("D3D name=%s result=%08lx\n", name, (unsigned long) hr);
    return SUCCEEDED(hr);
}
#include "ReplayRun.h"
int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2)
    {
        printf("usage: nvngx.dll.glass-replay.exe <manifest.json>\n");
        return 2;
    }
    const auto manifest = std::filesystem::absolute(argv[1]);
    std::ifstream manifestFile(manifest);
    if (!manifestFile)
    {
        printf("MANIFEST_OPEN_FAILED\n");
        return 2;
    }
    try
    {
        manifestFile >> replayConfig;
        outputWidth = replayConfig.at("outputWidth");
        outputHeight = replayConfig.at("outputHeight");
        renderWidth = replayConfig.at("renderWidth");
        renderHeight = replayConfig.at("renderHeight");
        replayGenerated = replayConfig.at("generatedCount");
        replayFrames = replayConfig.at("frames");
        sdkVersion = replayConfig.at("sdkVersion");
        applicationId = replayConfig.at("applicationId");
        if (replayConfig.at("formatProfile") != "rgba8-mv16f-depth32")
            throw std::runtime_error("unsupported format profile");
    }
    catch (const std::exception& e)
    {
        printf("MANIFEST_INVALID %s\n", e.what());
        return 2;
    }
    if (!replayFrames || replayFrames > 4096 || (replayGenerated != 1 && replayGenerated != 3) || !outputWidth ||
        !outputHeight || !renderWidth || !renderHeight || outputWidth > 16384 || outputHeight > 16384 ||
        renderWidth > 16384 || renderHeight > 16384)
        return 2;
    auto path = [&](const char* key)
    {
        return std::filesystem::absolute(manifest.parent_path() /
                                         std::filesystem::u8path(replayConfig.at(key).get<std::string>()));
    };
    std::filesystem::path provider, capture, packet, output, overrides, unlock;
    try
    {
        provider = path("provider");
        capture = path("capture");
        packet = path("arguments");
        output = path("output");
        if (replayConfig.contains("overrides"))
            overrides = path("overrides");
        if (replayGenerated == 3)
            unlock = path("unlock");
        if (!std::filesystem::is_regular_file(provider) || !std::filesystem::is_regular_file(packet) ||
            !std::filesystem::is_directory(capture) ||
            (replayGenerated == 3 && !std::filesystem::is_regular_file(unlock)) ||
            (!overrides.empty() && !std::filesystem::is_directory(overrides)))
            throw std::runtime_error("missing replay input");
    }
    catch (const std::exception& e)
    {
        printf("MANIFEST_PATH_INVALID %s\n", e.what());
        return 2;
    }
    if (std::filesystem::exists(output))
    {
        printf("OUTPUT_ALREADY_EXISTS\n");
        return 2;
    }
    if (replayGenerated == 3)
    {
        if (!LoadLibraryW(unlock.c_str()))
        {
            printf("UNLOCK_LOAD_FAILED error=%lu\n", GetLastError());
            return 13;
        }
        printf("UNLOCK_LOADED standalone_process_only=1\n");
    }
    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    if (!check(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**) &factory), "factory"))
        return 3;
    for (unsigned i = 0;; i++)
    {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 d {};
        adapter->GetDesc1(&d);
        if (d.VendorId == 0x10de && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**) &device);
        adapter->Release();
        if (device)
        {
            printf("ADAPTER vendor=%x device=%x\n", d.VendorId, d.DeviceId);
            break;
        }
    }
    factory->Release();
    if (!device)
        return 4;
    auto module = LoadLibraryW(provider.c_str());
    if (!module)
    {
        printf("LOAD_FAILED error=%lu\n", GetLastError());
        return 5;
    }
    using Init = unsigned (*)(unsigned long long, const wchar_t*, ID3D12Device*, unsigned, const void*);
    using Populate = unsigned (*)(void*);
    using Create = unsigned (*)(ID3D12GraphicsCommandList*, unsigned, void*, void**);
    using Release = unsigned (*)(void*);
    using Shutdown = unsigned (*)();
    auto init = (Init) GetProcAddress(module, "NVSDK_NGX_D3D12_Init_Ext");
    auto populate = (Populate) GetProcAddress(module, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
    auto create = (Create) GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature");
    auto release = (Release) GetProcAddress(module, "NVSDK_NGX_D3D12_ReleaseFeature");
    auto shutdown = (Shutdown) GetProcAddress(module, "NVSDK_NGX_D3D12_Shutdown");
    if (!init || !populate || !create || !release || !shutdown)
    {
        printf("MISSING_EXPORT\n");
        return 6;
    }
    wchar_t cwd[32768];
    GetCurrentDirectoryW(32768, cwd);
    auto result = init(applicationId, cwd, device, sdkVersion, nullptr);
    printf("INIT result=%08x\n", result);
    if (result != 1)
        return 7;
    Params p { table, {} };
    phase = "populate";
    result = populate(&p);
    printf("POPULATE result=%08x\n", result);
    if (result != 1)
    {
        shutdown();
        return 8;
    }
    if (replayGenerated == 3)
    {
        unsigned maximum = 0;
        if (get<unsigned, 4>(&p, "DLSSG.MultiFrameCountMax", &maximum) != 1 || maximum < 3)
        {
            printf("MULTIFRAME_CAPABILITY_FAILED maximum=%u\n", maximum);
            shutdown();
            return 14;
        }
    }
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    D3D12_COMMAND_QUEUE_DESC desc {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (!check(device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue), (void**) &queue), "queue") ||
        !check(device->CreateCommandAllocator(desc.Type, __uuidof(ID3D12CommandAllocator), (void**) &allocator),
               "allocator") ||
        !check(device->CreateCommandList(0, desc.Type, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList),
                                         (void**) &cmd),
               "command_list") ||
        !check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**) &fence), "fence"))
        return 9;
    phase = "configured_defaults_not_captured_creation";
    for (auto key : { "DLSSG.Width", "Width" })
        set<unsigned, 4>(&p, key, outputWidth);
    for (auto key : { "DLSSG.Height", "Height" })
        set<unsigned, 4>(&p, key, outputHeight);
    for (auto key :
         { "DLSSG.InternalWidth", "DLSSG.InternalHeight", "DLSSG.DynamicResolution", "DLSSG.UseReflexMatrices" })
        set<unsigned, 4>(&p, key, 0);
    set<unsigned, 4>(&p, "DLSSG.BackbufferFormat", DXGI_FORMAT_R8G8B8A8_UNORM);
    set<unsigned, 4>(&p, "CreationNodeMask", 1);
    set<unsigned, 4>(&p, "VisibilityNodeMask", 1);
    phase = "create";
    void* handle = nullptr;
    result = create(cmd, (unsigned) NVSDK_NGX_Feature_FrameGeneration, &p, &handle);
    printf("CREATE result=%08x handle=%p feature=%u\n", result, handle, (unsigned) NVSDK_NGX_Feature_FrameGeneration);
    // Creation may record uploads. Submit and wait before releasing the feature.
    bool flushed = check(cmd->Close(), "close");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (flushed && event)
    {
        ID3D12CommandList* lists[] = { cmd };
        queue->ExecuteCommandLists(1, lists);
        flushed =
            check(queue->Signal(fence, 1), "signal") && check(fence->SetEventOnCompletion(1, event), "fence_event");
        if (flushed)
        {
            auto wait = WaitForSingleObject(event, 30000);
            printf("WAIT result=%lu\n", wait);
            flushed = wait == WAIT_OBJECT_0;
        }
    }
    else
        flushed = false;
    if (!flushed)
    {
        printf("INCOMPLETE_GPU_DRAIN device_removed=%08lx\n", (unsigned long) device->GetDeviceRemovedReason());
        return 10;
    }
    int replayResult = 0;
    if (handle && result == 1)
    {
        try
        {
            ReplayRun runner(device, queue, allocator, cmd, fence);
            replayResult = runner.run(module, handle, p, capture.c_str(), packet.c_str(), output.c_str(),
                                      overrides.empty() ? nullptr : overrides.c_str());
        }
        catch (const std::exception& e)
        {
            printf("REPLAY_EXCEPTION %s\n", e.what());
            return 12;
        }
    }
    if (handle)
    {
        phase = "release";
        printf("RELEASE result=%08x\n", release(handle));
    }
    phase = "shutdown";
    printf("SHUTDOWN result=%08x\n", shutdown());
    CloseHandle(event);
    fence->Release();
    cmd->Release();
    allocator->Release();
    queue->Release();
    device->Release();
    printf("DONE mode=%s generated_count=%u replay_result=%d live_game_attachment=0 image_quality_verified=0\n",
           "replay", replayGenerated, replayResult);
    return replayResult ? replayResult : result == 1 ? 0 : 11;
}
