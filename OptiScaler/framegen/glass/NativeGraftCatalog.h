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
// SHA-256 of a shader container, from the same provider as the graft lookup. The
// pipeline cache calls it for the PS half of a pipeline's coverage identity
// (GeometryPipelineEntry::pixelHash), because the lookup above hashes only the
// VS. Returns false and a zeroed digest when the provider or the input is
// unusable. Same threading rule as FindNativeGraft: compiler worker only.
bool HashShaderSha256(const void* bytes, std::size_t size, std::array<std::uint8_t, 32>& digest) noexcept;
// Records in the loaded index; 0 when the index is absent or malformed.
std::size_t NativeGraftCount() noexcept;
// Why the catalog export gave a VS no record although it had a camera-only
// candidate (<module>/Glass/grafts/refused.bin, "GGREFS01", written by
// tools/export_native_grafts.py). VehicleObjectMotion: the VS draws vehicle
// geometry, which moves with its vehicle's transform; the engine's supply for
// that motion (MotionMatrix) does not reach this VS, and camera motion alone is
// wrong on a moving vehicle, so its draws keep the engine's motion.
enum class NativeGraftRefusal : std::uint32_t
{
    None = 0,
    VehicleObjectMotion = 1,
};
// Refusal for the VS digest FindNativeGraft reported; None when the VS is not
// listed or refused.bin is absent or malformed. Same threading rule as
// FindNativeGraft.
NativeGraftRefusal FindNativeGraftRefusal(const std::array<std::uint8_t, 32>& hash) noexcept;
// Refusals in the loaded refused.bin; 0 when absent or malformed.
std::size_t NativeGraftRefusalCount() noexcept;
// Pixel shaders all of whose engine technique passes draw light into the scene
// colour the display shows (<module>/Glass/grafts/light-ps.bin, "GGLTPS01",
// written by tools/export_native_grafts.py from the shader-cache census:
// transparent, unlit and screen-space VFX passes). Only for such a PS does the
// packed record opacity include the displayed brightness of the colour the
// draw adds (RewriteMaterialMotion lightTarget): what a distortion, decal,
// mark, depth or highlight pass writes is not light on screen. False for every
// PS when light-ps.bin is absent or malformed. Same threading rule as
// FindNativeGraft.
bool IsLightPixelShader(const std::array<std::uint8_t, 32>& sha256) noexcept;
// Load outcome of light-ps.bin for the module log: Pending until the first
// catalog lookup loads the files. count is 0 unless Loaded.
enum class LightPixelShaderList : std::uint32_t
{
    Pending,
    Loaded,
    Missing,
    Malformed,
};
LightPixelShaderList ReadLightPixelShaderList(std::size_t& count) noexcept;
} // namespace GlassFg
