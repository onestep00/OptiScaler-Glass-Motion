#pragma once
#include <d3d12.h>
#include <nvsdk_ngx_params.h>
#include <cstdint>

namespace GlassFg
{
// Live diagnostic `fgdump=N`: GPU copies of the frame generation output texture
// of the next N executed evaluations, written as PPM beside the packed dumps.
// Off unless requested; the normal path pays one relaxed load per evaluation
// and per observed submission.
//
// Thread model: the request comes from the live channel poll, recording from
// the evaluation thread, submission and command-list reset from the queue
// observer, allocation and file write from the health thread. One internal
// leaf mutex guards the batch; the log sink is never called while holding it.
using FgDumpLog = void (*)(const char* text) noexcept;
constexpr unsigned kFgDumpDefault = 8, kFgDumpMax = 64;

struct FgDumpPhase
{
    unsigned index = 0, count = 0;
    std::uint64_t frame = 0;
    // True when frame is the packed capture / Streamline frame of the
    // evaluation, false when it is the host evaluation counter.
    bool packedFrame = false;
    bool applied = false;
};

// False when a batch is already in progress.
bool RequestFgOutputDump(unsigned count, FgDumpLog log) noexcept;
bool FgOutputDumpWanted() noexcept;
// Evaluation thread, after the provider returned success on `command`.
// Records the output copy into that same command list, behind the provider's
// own work. Never allocates GPU memory.
void RecordFgOutputDump(ID3D12GraphicsCommandList* command, NVSDK_NGX_Parameter* parameters,
                        const FgDumpPhase& phase, FgDumpLog log) noexcept;
// Queue observer, after ExecuteCommandLists returned: signals the dump fence on
// the queue that carried a list with recorded copies.
void NoteFgOutputDumpSubmit(ID3D12CommandQueue* queue, unsigned count, ID3D12CommandList* const* lists,
                            FgDumpLog log) noexcept;
// Queue observer, after a command-list reset: a recorded copy whose list was
// reset without being submitted never executes.
void NoteFgOutputDumpReset(ID3D12GraphicsCommandList* command) noexcept;
// Lock-free: true while a batch can hold a copy recorded into a list that is
// still open (armed or draining), i.e. while NoteFgOutputDumpReset has to see
// the Reset of every list.
bool FgOutputDumpTracksResets() noexcept;
// Health thread: readback allocation, completion check, file write and release.
void ServiceFgOutputDump(FgDumpLog log) noexcept;
// Frame generation feature released or host stopped: no further phase is
// recorded; the phases already recorded are still written.
void RetireFgOutputDump(FgDumpLog log) noexcept;
} // namespace GlassFg
