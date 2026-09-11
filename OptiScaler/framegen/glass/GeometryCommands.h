#pragma once
#include "GraphicsRootBindings.h"
#include "GeometryDrawBatch.h"
#include "GeometryRasterState.h"

namespace GlassFg
{
struct GeometryDrawCaptureOwner;
struct GeometryCommandStats
{
    bool active = false;
    std::uint64_t recordings = 0, capacityRejected = 0, indexed = 0, packets = 0;
    std::uint64_t instances = 0, identities = 0, pipelinesReady = 0, bindingsReady = 0;
    std::uint64_t signatures = 0, indirectKnown = 0, indirectUnknown = 0;
    std::uint64_t captureRecorded = 0, captureRejected = 0;
    std::uint32_t lastFrame = 0;
};
// Startup-only, actual-device public method observation. Records CPU graphics
// bindings and consumes borrowed engine packets. Does not insert GPU commands.
bool StartGeometryCommands(ID3D12Device* device) noexcept;
GeometryCommandStats GetGeometryCommandStats() noexcept;
// Borrowed only during a synchronous callback on this live command. A caller
// must separately retain GPU resources through completion AND recording discard.
const GraphicsRootBindings* ReadGeometryBindings(ID3D12GraphicsCommandList* command) noexcept;
const GeometryRasterState* ReadGeometryRasterState(ID3D12GraphicsCommandList* command) noexcept;
std::uint64_t ReadGeometryRecordingEpoch(ID3D12GraphicsCommandList* command) noexcept;
// One process-resident owner, registered once. The owner handles admission and
// resource lifetime; this observer guarantees one original draw and restoration.
bool RegisterGeometryDrawCapture(GeometryDrawCaptureOwner* owner) noexcept;
} // namespace GlassFg
