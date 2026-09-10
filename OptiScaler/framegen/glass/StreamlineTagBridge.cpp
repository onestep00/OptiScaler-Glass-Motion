#include "pch.h"
#include "StreamlineTagBridge.h"
#include "TaggedInputs.h"
#include <sl.h>
#include <include/sl.param/parameters.h>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>

namespace GlassFg
{
namespace
{
// Read-only ABI projection of Streamline CommonResource and HashedResourceData.
// Source: NVIDIA-RTX/Streamline 2122257e0fce486f91b385aa63b9a09b0a34b363,
// source/plugins/sl.common/commonInterface.h and source/platforms/sl.chi/compute.h.
// The same offsets were observed on the installed 2.14.0 common plugin.
// No std::shared_ptr is copied, constructed, destroyed or retained across DLLs.
// The NVIDIA source is MIT licensed; see StreamlineTagBridge.LICENSE.txt.
struct CommonLayout
{
    uint64_t tagged;
    sl::Resource resource;
    sl::Extent extent;
    sl::PrecisionInfo precision;
    const void* cloneObject;
    const void* cloneCounter;
};
struct ClonePrefix
{
    uint64_t hash;
    uint32_t state, padding;
    const sl::Resource* resource;
};
static_assert(sizeof(sl::Resource) == 112 && sizeof(sl::PrecisionInfo) == 48);
static_assert(offsetof(CommonLayout, resource) == 8 && offsetof(CommonLayout, cloneObject) == 184);
static_assert(sizeof(CommonLayout) == 200 && offsetof(ClonePrefix, resource) == 16);

using GetTag = void (*)(unsigned, unsigned, unsigned, void*, const sl::BaseStructure**, unsigned, bool);
using Startup = bool (*)(const char*, void*);
using Shutdown = void (*)();
std::mutex mutex;
TaggedInputs taggedInputs;
sl::param::IParameters* parameters = nullptr;
Startup originalStartup = nullptr;
Shutdown originalShutdown = nullptr;
std::atomic<GetTag> originalGetTag = nullptr;
std::atomic<bool> observing = false;
bool supported = false;
HMODULE commonOwner = nullptr;

TaggedInputs::Tag decode(unsigned frame, unsigned viewport, const void* output)
{
    TaggedInputs::Tag tag;
    if (!output)
        return tag;
    CommonLayout data {};
    std::memcpy(&data, output, sizeof(data));
    // Only the source-defined clone path tested in native FG is supported.
    if (data.resource.structType != sl::Resource::s_structType || data.resource.structVersion != 1 ||
        data.resource.type != sl::ResourceType::eTex2d || data.resource.state != D3D12_RESOURCE_STATE_COPY_DEST ||
        !data.cloneObject || data.tagged == UINT64_MAX || data.tagged != frame)
        return tag;
    ClonePrefix clone {};
    std::memcpy(&clone, data.cloneObject, sizeof(clone));
    if (!clone.resource || clone.resource->structType != sl::Resource::s_structType ||
        clone.resource->structVersion != 1 || clone.resource->type != sl::ResourceType::eTex2d ||
        !clone.resource->native)
        return tag;
    tag.resource = clone.resource->native;
    tag.frame = data.tagged;
    tag.viewport = viewport;
    tag.state = data.resource.state;
    tag.left = data.extent.left;
    tag.top = data.extent.top;
    tag.width = data.extent.width;
    tag.height = data.extent.height;
    tag.valid = true;
    return tag;
}

void getTag(unsigned type, unsigned frame, unsigned viewport, void* output, const sl::BaseStructure** inputs,
            unsigned count, bool optional)
{
    // Never hold our lock while calling Streamline or change its output object.
    originalGetTag.load(std::memory_order_acquire)(type, frame, viewport, output, inputs, count, optional);
    if (type > 2 || !observing.load(std::memory_order_acquire))
        return;
    const auto tag = decode(frame, viewport, output);
    std::lock_guard lock(mutex);
    if (observing.load(std::memory_order_relaxed))
        taggedInputs.observe(type, tag);
}

bool startup(const char* config, void* device)
{
    {
        std::lock_guard lock(mutex);
        observing.store(false, std::memory_order_release);
        taggedInputs.clear();
    }
    const bool result = originalStartup(config, device);
    if (!result)
        return false;
    std::lock_guard lock(mutex);
    if (!supported || !parameters)
        return result;
    GetTag target = nullptr;
    if (!sl::param::getPointerParam(parameters, sl::param::global::kPFunGetTag, &target) || !target || target == getTag)
        return result;
    const auto previous = originalGetTag.load(std::memory_order_relaxed);
    // Other SL plugins can cache our wrapper. Never redirect those cached calls
    // to a different common implementation after OTA replacement in this process.
    if (previous && previous != target)
        return result;
    if (!commonOwner &&
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(target), &commonOwner))
        return result;
    originalGetTag.store(target, std::memory_order_release);
    parameters->set(sl::param::global::kPFunGetTag, reinterpret_cast<void*>(getTag));
    observing.store(true, std::memory_order_release);
    return result;
}

void shutdown()
{
    {
        std::lock_guard lock(mutex);
        observing.store(false, std::memory_order_release);
        taggedInputs.clear();
        if (parameters)
        {
            GetTag target = nullptr;
            if (sl::param::getPointerParam(parameters, sl::param::global::kPFunGetTag, &target) && target == getTag)
                parameters->set(sl::param::global::kPFunGetTag,
                                reinterpret_cast<void*>(originalGetTag.load(std::memory_order_relaxed)));
        }
    }
    originalShutdown();
}
} // namespace

void OnStreamlineCommonLoad(sl::param::IParameters* params, unsigned major, unsigned minor, unsigned patch)
{
    std::lock_guard lock(mutex);
    observing.store(false, std::memory_order_release);
    taggedInputs.clear();
    supported = params && major == 2 && minor == 14 && patch <= 1;
    parameters = params;
}

void* WrapStreamlineCommonFunction(const char* name, void* original)
{
    if (!name || !original)
        return original;
    std::lock_guard lock(mutex);
    if (!supported)
        return original;
    if (std::strcmp(name, "slOnPluginStartup") == 0)
    {
        originalStartup = reinterpret_cast<Startup>(original);
        return reinterpret_cast<void*>(startup);
    }
    if (std::strcmp(name, "slOnPluginShutdown") == 0)
    {
        originalShutdown = reinterpret_cast<Shutdown>(original);
        return reinterpret_cast<void*>(shutdown);
    }
    return original;
}

bool ReadStreamlineStates(const void* feature, unsigned index, unsigned count, ID3D12Resource* const (&resources)[3],
                          D3D12_RESOURCE_STATES (&states)[3])
{
    if (!observing.load(std::memory_order_acquire))
        return false;
    TaggedInputs::Tag tags[3];
    const void* identities[] = { resources[0], resources[1], resources[2] };
    {
        std::lock_guard lock(mutex);
        if (!observing.load(std::memory_order_relaxed) || !taggedInputs.read(feature, index, count, identities, tags))
            return false;
    }
    // These are the live native Evaluate parameters, not retained tag pointers.
    for (unsigned i = 0; i < 3; ++i)
    {
        const auto desc = resources[i]->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) ||
            (tags[i].width && tags[i].width != desc.Width) || (tags[i].height && tags[i].height != desc.Height))
            return false;
    }
    for (unsigned i = 0; i < 3; ++i)
        states[i] = static_cast<D3D12_RESOURCE_STATES>(tags[i].state);
    return true;
}
} // namespace GlassFg
