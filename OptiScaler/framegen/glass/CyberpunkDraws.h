#pragma once
#include "GeometryDrawBatch.h"
#include <windows.h>

namespace GlassFg
{
// Startup-only acquisition of audited renderer batch/append/flush paths.
bool InitializeCyberpunkDraws(HMODULE executable) noexcept;
// Called only from an actual public DrawIndexedInstanced observer. The source
// return address must be the audited engine call site. The borrowed result is
// valid only inside this callback; a GPU owner must copy admitted identities.
GeometryDrawView ReadCyberpunkGeometryDraw(const void* sourceReturnAddress, std::uint32_t indexCount,
                                           std::uint32_t instanceCount, std::uint32_t startIndex,
                                           std::int32_t baseVertex, std::uint32_t startInstance) noexcept;
struct CyberpunkDrawStatus
{
    bool active = false;
    std::uint64_t batches = 0, appends = 0, identities = 0, draws = 0, rejected = 0;
};
CyberpunkDrawStatus GetCyberpunkDrawStatus() noexcept;
} // namespace GlassFg
