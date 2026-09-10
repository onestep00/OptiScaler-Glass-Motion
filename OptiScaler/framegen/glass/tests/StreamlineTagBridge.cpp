#include "pch.h"
#include <string>
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include "../StreamlineTagBridge.cpp"

static void check(bool value)
{
    if (!value)
        throw std::runtime_error("Streamline bridge contract failed");
}
struct Parameters : sl::param::IParameters
{
    void* function = nullptr;
    unsigned writes = 0;
#define VALUE(T)                                                                                                       \
    void set(const char*, T) override {}                                                                               \
    bool get(const char*, T*) const override { return false; }
    VALUE(bool)
    VALUE(unsigned long long) VALUE(float) VALUE(double) VALUE(unsigned int) VALUE(int)
#undef VALUE
        void set(const char* key, void* value) override
    {
        check(std::strcmp(key, sl::param::global::kPFunGetTag) == 0);
        function = value;
        ++writes;
    }
    bool get(const char* key, void** value) const override
    {
        if (std::strcmp(key, sl::param::global::kPFunGetTag))
            return false;
        *value = function;
        return true;
    }
    std::vector<std::string> enumerate() const override { return {}; }
};
static Parameters table;
static GlassFg::CommonLayout fixture[3] {};
static GlassFg::ClonePrefix clones[3] {};
static sl::Resource metadata[3] {};
static bool startupOk = true;
static unsigned calls = 0, stops = 0;
static void original(unsigned type, unsigned, unsigned, void* output, const sl::BaseStructure** inputs, unsigned count,
                     bool optional)
{
    check(!inputs && count == 0 && !optional && type < 3);
    ++calls;
    std::memcpy(output, &fixture[type], sizeof(fixture[type]));
}
static bool start(const char* config, void* device)
{
    check(std::strcmp(config, "unchanged") == 0 && device == &table);
    if (startupOk)
        table.set(sl::param::global::kPFunGetTag, reinterpret_cast<void*>(original));
    return startupOk;
}
static void stop()
{
    check(table.function == reinterpret_cast<void*>(original));
    ++stops;
}
int main()
{
    using namespace GlassFg;
    ID3D12Device* device = nullptr;
    check(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))));
    ID3D12Resource* resources[3] {};
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 64;
    desc.Height = 32;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (auto& resource : resources)
        check(SUCCEEDED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource))));
    unsigned order[] = { 2, 0, 1 };
    for (unsigned i = 0; i < 3; ++i)
    {
        metadata[i] = sl::Resource(sl::ResourceType::eTex2d, resources[order[i]], 0x400);
        clones[i].resource = &metadata[i];
        fixture[i].resource = sl::Resource(sl::ResourceType::eTex2d, nullptr, 0x400);
        fixture[i].extent = { 0, 0, 64, 32 };
        fixture[i].cloneObject = &clones[i];
    }
    OnStreamlineCommonLoad(&table, 2, 15, 0);
    check(WrapStreamlineCommonFunction("slOnPluginStartup", reinterpret_cast<void*>(start)) ==
          reinterpret_cast<void*>(start));
    OnStreamlineCommonLoad(&table, 2, 14, 0);
    auto startBridge =
        reinterpret_cast<Startup>(WrapStreamlineCommonFunction("slOnPluginStartup", reinterpret_cast<void*>(start)));
    auto stopBridge =
        reinterpret_cast<Shutdown>(WrapStreamlineCommonFunction("slOnPluginShutdown", reinterpret_cast<void*>(stop)));
    startupOk = false;
    check(!startBridge("unchanged", &table) && !table.function);
    startupOk = true;
    check(startBridge("unchanged", &table) && table.function != reinterpret_cast<void*>(original));
    auto reader = reinterpret_cast<GetTag>(table.function);
    auto feed = [&](unsigned frame)
    {
        for (unsigned i = 0; i < 3; ++i)
        {
            fixture[i].tagged = frame;
            CommonLayout output {};
            reader(i, frame, 0, &output, nullptr, 0, false);
            check(std::memcmp(&output, &fixture[i], sizeof(output)) == 0);
        }
    };
    std::thread producer([&] { feed(1); });
    producer.join();
    D3D12_RESOURCE_STATES states[3] {};
    int feature;
    check(ReadStreamlineStates(&feature, 1, 3, resources, states));
    check(ReadStreamlineStates(&feature, 2, 3, resources, states));
    check(ReadStreamlineStates(&feature, 3, 3, resources, states));
    check(states[0] == 0x400 && states[1] == 0x400 && states[2] == 0x400);
    check(!ReadStreamlineStates(&feature, 1, 3, resources, states));
    fixture[2].resource.structVersion = 99;
    feed(2);
    check(!ReadStreamlineStates(&feature, 1, 3, resources, states));
    fixture[2].resource.structVersion = 1;
    fixture[2].extent.width = 32;
    feed(3);
    check(!ReadStreamlineStates(&feature, 1, 3, resources, states));
    fixture[2].extent.width = 64;
    feed(4);
    check(ReadStreamlineStates(&feature, 1, 1, resources, states));
    stopBridge();
    check(stops == 1 && !ReadStreamlineStates(&feature, 1, 1, resources, states));
    // A plugin that cached the wrapper retains forwarding semantics after stop.
    auto previousCalls = calls;
    feed(5);
    check(calls == previousCalls + 3);
    for (auto resource : resources)
        resource->Release();
    device->Release();
    std::puts("STREAMLINE_BRIDGE registration=pass forwarding=pass cross_thread=pass states=pass rejection=pass "
              "shutdown=pass");
}
