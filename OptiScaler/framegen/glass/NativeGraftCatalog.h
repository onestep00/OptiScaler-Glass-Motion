#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace GlassFg
{
// Grafted vertex shader for one original transparent VS. The graft keeps every
// original output and adds the engine's de-jittered current clip and its
// previous clip (MotionMatrix rows 24..26 of the material constant buffer) as
// the float4 outputs currentOutput/previousOutput. supplyClass carries the
// GraftClass* bits of what the previous graph reads (GlassControls.h).
// cameraBytes, when non-null, is the camera-only variant of the same original VS
// for instanced array draws: its cameraPreviousOutput applies the native previous
// view-projection (b1 rows 16..19 or 12..15) to the VS's own current world
// position and reads no b7 row. cameraCurrentOutput is the same de-jittered
// current clip. A camera-only record (a VS with no native current-position twin)
// has no root graft: bytes is null, size and the root outputs are 0, and the
// camera variant serves every draw of the VS. supplyClass then is the class of
// the VS's own current position cone. bytes and cameraBytes stay valid for the
// process lifetime.
struct NativeGraft
{
    const void* bytes;
    std::size_t size;
    unsigned currentOutput, previousOutput, supplyClass;
    const void* cameraBytes;
    std::size_t cameraSize;
    unsigned cameraCurrentOutput, cameraPreviousOutput;
};
// Looks up the original VS by the SHA-256 of its container bytes in
// <module>/Glass/grafts/index.bin ("GGRAFT02", sorted on first use) and loads
// the matching <sha>.dxil (and <sha>.camera.dxil when indexed) on its first hit.
// Thread-safe; the index is read once
// and a miss after that performs no allocation. hash, when given, receives the
// digest for the caller's diagnostics (all zero if hashing failed).
// Pipeline-cache compiler worker only: hashing and file I/O never belong in a
// draw or API callback.
std::optional<NativeGraft> FindNativeGraft(const void* vertexShader, std::size_t size,
                                           std::array<std::uint8_t, 32>* hash = nullptr) noexcept;
// Records in the loaded index; 0 when the index is absent or malformed.
std::size_t NativeGraftCount() noexcept;
} // namespace GlassFg
