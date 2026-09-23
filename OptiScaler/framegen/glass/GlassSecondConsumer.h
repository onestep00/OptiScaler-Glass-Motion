#pragma once

#include <d3d12.h>

namespace GlassFg
{
// Second consumer: the DLSS 5 neural rendering pass.
//
// The pass reads the game's depth and motion vectors from its own evaluate
// parameters. This hands back the corrected transparent-object pair for the
// engine frame in flight instead, composed inline on the caller's command list
// because that pass runs before the frame generation batch is submitted.
//
// Answers false when the option is off, when no composed pair exists for this
// frame, or when the caller's textures are not the extent and format the
// compose was built for. False means the caller keeps its own guides.
//
// The engine's own motion and depth textures are never written; only this
// evaluate's inputs change.
//
// motionArrival/depthArrival are the states the game left those two textures
// in, as the caller reads them from its own parameter block. The compose copies
// from them, so its transitions have to start from the real state; the caller
// must pass what it declares rather than the NGX default, or the transition is
// recorded from a state the resource is not in.
//
// jitterX/jitterY are the frame's sub-pixel projection jitter in pixels, in the
// same convention the frame generation channel declares. The delivered motion
// converts the captured jittered-projection value back to the engine's
// jitter-free convention with them, exactly as the frame generation path does.
bool SecondConsumerGuides(ID3D12GraphicsCommandList* command, ID3D12Resource* motion, ID3D12Resource* depth,
                          D3D12_RESOURCE_STATES motionArrival, D3D12_RESOURCE_STATES depthArrival, float jitterX,
                          float jitterY, float scaleX, float scaleY, ID3D12Resource** outMotion,
                          ID3D12Resource** outDepth) noexcept;
} // namespace GlassFg
