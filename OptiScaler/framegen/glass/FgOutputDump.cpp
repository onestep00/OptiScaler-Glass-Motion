#include "pch.h"
#include "FgOutputDump.h"
#include "D3D12Observer.h"
#include "MotionDumpFormat.h"
#include <Util.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <wrl/client.h>

namespace GlassFg
{
namespace
{
// Parameter names the output texture is looked up under, in order. The game
// level DLSS-G table names it DLSSG.OutputInterpolated (the key OptiScaler's
// own DLSS-G providers read, nvngx/Nvngx_FFX.cpp); the driver-level table the
// Streamline plugin hands to the NGX core may use a generic name instead.
const char* const kOutputKeys[] { "DLSSG.OutputInterpolated", "Output", "DLSSG.Output", "OutputInterpolated" };
// State the provider leaves the output in when the evaluate returns: DLSS-G
// writes it as a UAV and the recorded replay reads it back from exactly that
// state (tests/replay/ReplayRun.h, UNORDERED_ACCESS -> COPY_SOURCE).
constexpr D3D12_RESOURCE_STATES kOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
// A batch that makes no progress for this long is truncated or abandoned.
constexpr std::uint64_t kDeadlineMs = 15000;

enum class Phase : unsigned
{
    Idle,
    Observing,  // waiting for an evaluation that names the output texture
    Allocating, // health thread sizes and creates the readbacks
    Armed,      // evaluations record copies
    Draining,   // every copy recorded; waiting for submission and the fence
    Writing,    // health thread owns the batch
};

enum class SlotState : unsigned char
{
    Empty,
    Recorded, // copy recorded into an open command list
    Signaled, // that list was submitted; fenceValue covers it
    Dropped,  // the list was reset without being submitted
};

struct Slot
{
    ID3D12Resource* readback = nullptr;
    SlotState state = SlotState::Empty;
    ID3D12GraphicsCommandList* command = nullptr;
    std::uint64_t fenceValue = 0;
    FgDumpPhase phase {};
    ID3D12Resource* resource = nullptr; // identity only, for the manifest
    const char* key = nullptr;
};

struct State
{
    std::mutex mutex;
    std::atomic<Phase> phase { Phase::Idle };
    unsigned requested = 0, allocated = 0, recorded = 0, serial = 0;
    const char* key = nullptr;
    D3D12_RESOURCE_DESC description {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    ID3D12Fence* fence = nullptr;
    std::uint64_t fenceValue = 0;
    std::uint64_t sinceMs = 0;
    bool missingLogged = false, shapeLogged = false, keyLogged = false, stallLogged = false;
    Slot slots[kFgDumpMax] {};
};

// Never destroyed: hooks can outlive static destruction.
State& state()
{
    static auto* value = new State;
    return *value;
}

// Log lines collected under the batch mutex and emitted after it is released,
// so the sink (which takes the host mutex) never nests inside it.
struct Lines
{
    char text[1536] {};
    std::size_t used = 0;
    void add(const char* format, ...) noexcept
    {
        if (used + 1 >= sizeof(text))
            return;
        va_list arguments;
        va_start(arguments, format);
        const int written = std::vsnprintf(text + used, sizeof(text) - used, format, arguments);
        va_end(arguments);
        if (written > 0)
            used = (std::min)(sizeof(text) - 1, used + static_cast<std::size_t>(written));
    }
    void flush(FgDumpLog log) noexcept
    {
        if (log && used)
            log(text);
        used = 0;
        text[0] = 0;
    }
};

bool active(Phase phase) noexcept { return phase == Phase::Armed || phase == Phase::Draining; }

// Called with the mutex held and only when no recorded copy can still execute.
void releaseLocked(State& s) noexcept
{
    for (auto& slot : s.slots)
    {
        if (slot.readback)
            slot.readback->Release();
        slot = Slot {};
    }
    if (s.fence)
        s.fence->Release();
    s.fence = nullptr;
    s.fenceValue = 0;
    s.device.Reset();
    s.allocated = 0;
    s.recorded = 0;
    s.phase.store(Phase::Idle, std::memory_order_release);
}

void transition(ID3D12GraphicsCommandList* command, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) noexcept
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    command->ResourceBarrier(1, &barrier);
}

bool sameShape(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) noexcept
{
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.Dimension == b.Dimension &&
           a.SampleDesc.Count == b.SampleDesc.Count;
}
} // namespace

bool RequestFgOutputDump(unsigned count, FgDumpLog log) noexcept
{
    auto& s = state();
    Lines lines;
    bool accepted = false;
    {
        std::lock_guard lock(s.mutex);
        if (s.phase.load(std::memory_order_acquire) == Phase::Idle)
        {
            s.requested = count == 0 ? kFgDumpDefault : (std::min)(count, kFgDumpMax);
            s.recorded = 0;
            s.key = nullptr;
            s.missingLogged = s.shapeLogged = s.keyLogged = s.stallLogged = false;
            s.sinceMs = GetTickCount64();
            s.phase.store(Phase::Observing, std::memory_order_release);
            accepted = true;
            lines.add("GLASS_FGDUMP requested count=%u\n", s.requested);
        }
        else
            lines.add("GLASS_FGDUMP busy phase=%u\n", static_cast<unsigned>(s.phase.load()));
    }
    lines.flush(log);
    return accepted;
}

bool FgOutputDumpWanted() noexcept
{
    const auto phase = state().phase.load(std::memory_order_relaxed);
    return phase == Phase::Observing || phase == Phase::Armed;
}

void RecordFgOutputDump(ID3D12GraphicsCommandList* command, NVSDK_NGX_Parameter* parameters,
                        const FgDumpPhase& phase, FgDumpLog log) noexcept
{
    if (!command || !parameters || !FgOutputDumpWanted())
        return;
    auto& s = state();
    Lines lines;
    {
        std::lock_guard lock(s.mutex);
        const auto current = s.phase.load(std::memory_order_acquire);
        if (current != Phase::Observing && current != Phase::Armed)
            return;
        const char* key = nullptr;
        ID3D12Resource* output = nullptr;
        for (const char* candidate : kOutputKeys)
        {
            ID3D12Resource* value = nullptr;
            if (parameters->Get(candidate, &value) == NVSDK_NGX_Result_Success && value != nullptr)
            {
                key = candidate;
                output = value;
                break;
            }
        }
        if (!output)
        {
            if (!s.missingLogged)
            {
                s.missingLogged = true;
                lines.add("GLASS_FGDUMP missing_output index=%u count=%u\n", phase.index, phase.count);
            }
        }
        else if (current == Phase::Observing)
        {
            const auto description = output->GetDesc();
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            if (SUCCEEDED(output->GetDevice(IID_PPV_ARGS(&device))))
            {
                s.device = device;
                s.description = description;
                s.key = key;
                s.sinceMs = GetTickCount64();
                s.phase.store(Phase::Allocating, std::memory_order_release);
                lines.add("GLASS_FGDUMP key=%s resource=%p format=%u extent=%llux%u state_before=%u\n", key,
                          static_cast<void*>(output), static_cast<unsigned>(description.Format),
                          static_cast<unsigned long long>(description.Width), description.Height,
                          static_cast<unsigned>(kOutputState));
            }
        }
        else if (s.recorded < s.allocated)
        {
            const auto description = output->GetDesc();
            if (!sameShape(description, s.description))
            {
                if (!s.shapeLogged)
                {
                    s.shapeLogged = true;
                    lines.add("GLASS_FGDUMP skip reason=shape_changed format=%u extent=%llux%u allocated=%u %llux%u\n",
                              static_cast<unsigned>(description.Format),
                              static_cast<unsigned long long>(description.Width), description.Height,
                              static_cast<unsigned>(s.description.Format),
                              static_cast<unsigned long long>(s.description.Width), s.description.Height);
                }
            }
            else
            {
                if (key != s.key && !s.keyLogged)
                {
                    s.keyLogged = true;
                    lines.add("GLASS_FGDUMP key=%s changed_from=%s\n", key, s.key ? s.key : "-");
                }
                auto& slot = s.slots[s.recorded];
                {
                    // Behind the provider's own work in the same list, so the
                    // copy reads exactly the texture this phase produced.
                    InternalD3D12Scope ownCalls;
                    transition(command, output, kOutputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION source {}, target {};
                    source.pResource = output;
                    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    source.SubresourceIndex = 0;
                    target.pResource = slot.readback;
                    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    target.PlacedFootprint = s.footprint;
                    command->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
                    transition(command, output, D3D12_RESOURCE_STATE_COPY_SOURCE, kOutputState);
                }
                slot.state = SlotState::Recorded;
                slot.command = command;
                slot.fenceValue = 0;
                slot.phase = phase;
                slot.resource = output;
                slot.key = key;
                lines.add("GLASS_FGDUMP recorded serial=%u seq=%u index=%u count=%u frame=%llu applied=%u\n", s.serial,
                          s.recorded, phase.index, phase.count, static_cast<unsigned long long>(phase.frame),
                          phase.applied ? 1u : 0u);
                if (++s.recorded == s.allocated)
                {
                    s.sinceMs = GetTickCount64();
                    s.phase.store(Phase::Draining, std::memory_order_release);
                }
            }
        }
    }
    lines.flush(log);
}

void NoteFgOutputDumpSubmit(ID3D12CommandQueue* queue, unsigned count, ID3D12CommandList* const* lists,
                            FgDumpLog log) noexcept
{
    auto& s = state();
    if (!queue || !lists || !active(s.phase.load(std::memory_order_relaxed)))
        return;
    Lines lines;
    {
        std::lock_guard lock(s.mutex);
        if (!active(s.phase.load(std::memory_order_acquire)) || !s.fence)
            return;
        bool carried = false;
        for (unsigned i = 0; i < s.recorded && !carried; ++i)
        {
            const auto& slot = s.slots[i];
            if (slot.state != SlotState::Recorded)
                continue;
            for (unsigned j = 0; j < count && !carried; ++j)
                carried = lists[j] == slot.command;
        }
        if (!carried)
            return;
        HRESULT signaled = E_FAIL;
        {
            InternalD3D12Scope ownSignal;
            signaled = queue->Signal(s.fence, s.fenceValue + 1);
        }
        if (FAILED(signaled))
        {
            lines.add("GLASS_FGDUMP signal_failed serial=%u hr=0x%08lx\n", s.serial, static_cast<unsigned long>(signaled));
        }
        else
        {
            ++s.fenceValue;
            unsigned covered = 0;
            for (unsigned i = 0; i < s.recorded; ++i)
            {
                auto& slot = s.slots[i];
                if (slot.state != SlotState::Recorded)
                    continue;
                for (unsigned j = 0; j < count; ++j)
                    if (lists[j] == slot.command)
                    {
                        slot.state = SlotState::Signaled;
                        slot.fenceValue = s.fenceValue;
                        ++covered;
                        break;
                    }
            }
            lines.add("GLASS_FGDUMP submitted serial=%u slots=%u value=%llu queue=%p\n", s.serial, covered,
                      static_cast<unsigned long long>(s.fenceValue), static_cast<void*>(queue));
        }
    }
    lines.flush(log);
}

void NoteFgOutputDumpReset(ID3D12GraphicsCommandList* command) noexcept
{
    auto& s = state();
    if (!command || !active(s.phase.load(std::memory_order_relaxed)))
        return;
    std::lock_guard lock(s.mutex);
    if (!active(s.phase.load(std::memory_order_acquire)))
        return;
    for (unsigned i = 0; i < s.recorded; ++i)
        if (s.slots[i].state == SlotState::Recorded && s.slots[i].command == command)
            s.slots[i].state = SlotState::Dropped;
}

void RetireFgOutputDump(FgDumpLog log) noexcept
{
    auto& s = state();
    const auto current = s.phase.load(std::memory_order_relaxed);
    if (current == Phase::Idle || current == Phase::Writing)
        return;
    Lines lines;
    {
        std::lock_guard lock(s.mutex);
        const auto phase = s.phase.load(std::memory_order_acquire);
        if (phase == Phase::Observing || phase == Phase::Allocating)
        {
            // Allocating: the health thread sees the phase change when it
            // relocks and releases what it created itself.
            s.device.Reset();
            s.phase.store(Phase::Idle, std::memory_order_release);
            lines.add("GLASS_FGDUMP aborted reason=retired phase=%u\n", static_cast<unsigned>(phase));
        }
        else if (phase == Phase::Armed)
        {
            if (s.recorded == 0)
            {
                releaseLocked(s);
                lines.add("GLASS_FGDUMP aborted reason=retired serial=%u recorded=0\n", s.serial);
            }
            else
            {
                s.sinceMs = GetTickCount64();
                s.phase.store(Phase::Draining, std::memory_order_release);
                lines.add("GLASS_FGDUMP truncated reason=retired serial=%u recorded=%u requested=%u\n", s.serial,
                          s.recorded, s.requested);
            }
        }
    }
    lines.flush(log);
}

void ServiceFgOutputDump(FgDumpLog log) noexcept
{
    auto& s = state();
    const auto observed = s.phase.load(std::memory_order_acquire);
    if (observed == Phase::Idle || observed == Phase::Writing)
        return;
    Lines lines;
    try
    {
        std::unique_lock lock(s.mutex);
        const auto now = GetTickCount64();
        const auto phase = s.phase.load(std::memory_order_acquire);
        if (phase == Phase::Observing)
        {
            if (now - s.sinceMs > kDeadlineMs)
            {
                s.device.Reset();
                s.phase.store(Phase::Idle, std::memory_order_release);
                lines.add("GLASS_FGDUMP aborted reason=%s\n", s.missingLogged ? "missing_output" : "no_evaluation");
            }
        }
        else if (phase == Phase::Allocating)
        {
            // Sized and created here, outside the lock and off the render
            // thread; the evaluation only records into what exists.
            const auto description = s.description;
            const auto requested = s.requested;
            Microsoft::WRL::ComPtr<ID3D12Device> device = s.device;
            lock.unlock();
            if (!ColorPpmSupported(description.Format))
            {
                lock.lock();
                if (s.phase.load(std::memory_order_acquire) == Phase::Allocating)
                {
                    s.device.Reset();
                    s.phase.store(Phase::Idle, std::memory_order_release);
                }
                lines.add("GLASS_FGDUMP skip reason=unknown_format format=%u\n",
                          static_cast<unsigned>(description.Format));
            }
            else
            {
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
                UINT64 bytes = 0;
                device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
                ID3D12Fence* fence = nullptr;
                ID3D12Resource* created[kFgDumpMax] {};
                unsigned count = 0;
                if (bytes != 0 && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                {
                    D3D12_HEAP_PROPERTIES properties {};
                    properties.Type = D3D12_HEAP_TYPE_READBACK;
                    D3D12_RESOURCE_DESC buffer {};
                    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    buffer.Width = bytes;
                    buffer.Height = 1;
                    buffer.DepthOrArraySize = 1;
                    buffer.MipLevels = 1;
                    buffer.SampleDesc.Count = 1;
                    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    while (count < requested &&
                           SUCCEEDED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                     IID_PPV_ARGS(&created[count]))))
                        ++count;
                }
                lock.lock();
                if (s.phase.load(std::memory_order_acquire) != Phase::Allocating || count == 0)
                {
                    // Retired meanwhile, or nothing could be created: nothing
                    // was recorded against these, so they go straight back.
                    for (unsigned i = 0; i < count; ++i)
                        created[i]->Release();
                    if (fence)
                        fence->Release();
                    if (s.phase.load(std::memory_order_acquire) == Phase::Allocating)
                    {
                        s.device.Reset();
                        s.phase.store(Phase::Idle, std::memory_order_release);
                        lines.add("GLASS_FGDUMP aborted reason=allocation_failed bytes=%llu\n",
                                  static_cast<unsigned long long>(bytes));
                    }
                }
                else
                {
                    for (unsigned i = 0; i < count; ++i)
                        s.slots[i] = Slot { created[i] };
                    s.fence = fence;
                    s.fenceValue = 0;
                    s.footprint = footprint;
                    s.allocated = count;
                    s.recorded = 0;
                    ++s.serial;
                    s.sinceMs = GetTickCount64();
                    s.phase.store(Phase::Armed, std::memory_order_release);
                    lines.add("GLASS_FGDUMP armed serial=%u slots=%u requested=%u bytes_per_slot=%llu\n", s.serial,
                              count, requested, static_cast<unsigned long long>(bytes));
                }
            }
        }
        else if (phase == Phase::Armed)
        {
            if (now - s.sinceMs > kDeadlineMs)
            {
                if (s.recorded == 0)
                {
                    releaseLocked(s);
                    lines.add("GLASS_FGDUMP aborted reason=no_evaluation serial=%u\n", s.serial);
                }
                else
                {
                    s.sinceMs = now;
                    s.phase.store(Phase::Draining, std::memory_order_release);
                    lines.add("GLASS_FGDUMP truncated reason=timeout serial=%u recorded=%u requested=%u\n", s.serial,
                              s.recorded, s.requested);
                }
            }
        }
        else if (phase == Phase::Draining)
        {
            const auto completed = s.fence ? s.fence->GetCompletedValue() : 0;
            if (completed == UINT64_MAX)
            {
                // Device removed: no copy can still run, and none can be read.
                releaseLocked(s);
                lines.add("GLASS_FGDUMP aborted reason=device_removed serial=%u\n", s.serial);
            }
            else
            {
                unsigned waiting = 0, unsubmitted = 0;
                for (unsigned i = 0; i < s.recorded; ++i)
                {
                    const auto& slot = s.slots[i];
                    if (slot.state == SlotState::Recorded)
                        ++unsubmitted;
                    else if (slot.state == SlotState::Signaled && completed < slot.fenceValue)
                        ++waiting;
                }
                if (waiting != 0 || unsubmitted != 0)
                {
                    // The readbacks stay allocated while any recorded copy can
                    // still execute; the stall is reported once.
                    if (now - s.sinceMs > kDeadlineMs && !s.stallLogged)
                    {
                        s.stallLogged = true;
                        lines.add("GLASS_FGDUMP waiting serial=%u unsubmitted=%u incomplete=%u completed=%llu "
                                  "need=%llu\n",
                                  s.serial, unsubmitted, waiting, static_cast<unsigned long long>(completed),
                                  static_cast<unsigned long long>(s.fenceValue));
                    }
                }
                else
                {
                    s.phase.store(Phase::Writing, std::memory_order_release);
                    lock.unlock();
                    // Writing: no other thread touches the batch until Idle.
                    const auto folder = Util::DllPath().parent_path() / L"Glass";
                    std::error_code error;
                    std::filesystem::create_directories(folder, error);
                    const auto serial = std::to_wstring(s.serial);
                    const auto manifestPath = folder / (L"fgdump-" + serial + L".txt");
                    FILE* manifest = _wfopen(manifestPath.c_str(), L"w");
                    const auto& description = s.description;
                    if (manifest)
                        std::fprintf(manifest,
                                     "serial=%u requested=%u recorded=%u key=%s format=%u extent=%llux%u "
                                     "state_before=%u\n",
                                     s.serial, s.requested, s.recorded, s.key ? s.key : "-",
                                     static_cast<unsigned>(description.Format),
                                     static_cast<unsigned long long>(description.Width), description.Height,
                                     static_cast<unsigned>(kOutputState));
                    unsigned written = 0;
                    for (unsigned i = 0; i < s.recorded; ++i)
                    {
                        const auto& slot = s.slots[i];
                        const auto name = L"fgdump-" + serial + L"-" + std::to_wstring(i) + L"-i" +
                                          std::to_wstring(slot.phase.index) + L"-c" +
                                          std::to_wstring(slot.phase.count) + L"-f" +
                                          std::to_wstring(slot.phase.frame) + L".ppm";
                        const char* status = "dropped";
                        if (slot.state == SlotState::Signaled)
                        {
                            void* data = nullptr;
                            status = "map_failed";
                            if (SUCCEEDED(slot.readback->Map(0, nullptr, &data)) && data)
                            {
                                const bool okay = WriteColorPpm(
                                    (folder / name).c_str(), static_cast<const std::byte*>(data) + s.footprint.Offset,
                                    description.Format, static_cast<unsigned>(description.Width),
                                    description.Height, s.footprint.Footprint.RowPitch);
                                D3D12_RANGE none { 0, 0 };
                                slot.readback->Unmap(0, &none);
                                status = okay ? "written" : "write_failed";
                                written += okay ? 1u : 0u;
                            }
                        }
                        if (manifest)
                            std::fprintf(manifest,
                                         "seq=%u index=%u count=%u frame=%llu frame_source=%s applied=%u format=%u "
                                         "extent=%llux%u resource=%p key=%s status=%s file=%ls\n",
                                         i, slot.phase.index, slot.phase.count,
                                         static_cast<unsigned long long>(slot.phase.frame),
                                         slot.phase.packedFrame ? "packed" : "host", slot.phase.applied ? 1u : 0u,
                                         static_cast<unsigned>(description.Format),
                                         static_cast<unsigned long long>(description.Width), description.Height,
                                         static_cast<void*>(slot.resource), slot.key ? slot.key : "-", status,
                                         name.c_str());
                    }
                    if (manifest)
                        std::fclose(manifest);
                    lines.add("FGDUMP written serial=%u count=%u recorded=%u requested=%u path=%ls\n", s.serial,
                              written, s.recorded, s.requested, manifestPath.c_str());
                    lock.lock();
                    releaseLocked(s);
                }
            }
        }
    }
    catch (...)
    {
        // Only the filesystem / string work can throw; the batch is finished
        // either way so the live channel is not left waiting.
        std::lock_guard lock(s.mutex);
        if (s.phase.load(std::memory_order_acquire) == Phase::Writing)
            releaseLocked(s);
        lines.add("GLASS_FGDUMP aborted reason=exception serial=%u\n", s.serial);
    }
    lines.flush(log);
}
} // namespace GlassFg
