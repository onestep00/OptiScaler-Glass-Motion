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
// GraftClass* bits of what the previous graph reads (GlassControls.h). bytes
// stay valid for the process lifetime.
struct NativeGraft
{
    const void* bytes;
    std::size_t size;
    unsigned currentOutput, previousOutput, supplyClass;
};
// Looks up the original VS by the SHA-256 of its container bytes in
// <module>/Glass/grafts/index.bin ("GGRAFT01", sorted on first use) and loads
// the matching <sha>.dxil on its first hit. Thread-safe; the index is read once
// and a miss after that performs no allocation. hash, when given, receives the
// digest for the caller's diagnostics (all zero if hashing failed).
// Pipeline-cache compiler worker only: hashing and file I/O never belong in a
// draw or API callback.
std::optional<NativeGraft> FindNativeGraft(const void* vertexShader, std::size_t size,
                                           std::array<std::uint8_t, 32>* hash = nullptr) noexcept;
// Records in the loaded index; 0 when the index is absent or malformed.
std::size_t NativeGraftCount() noexcept;
} // namespace GlassFg
