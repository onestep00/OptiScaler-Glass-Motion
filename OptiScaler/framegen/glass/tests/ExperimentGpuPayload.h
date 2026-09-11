#pragma once
#include <d3d12.h>
// Independent fixture payload only; this is not the game input ABI.
struct ExperimentGpuPayload
{
    ID3D12GraphicsCommandList* command;
    ID3D12Resource* destination;
    UINT64 offset;
};
