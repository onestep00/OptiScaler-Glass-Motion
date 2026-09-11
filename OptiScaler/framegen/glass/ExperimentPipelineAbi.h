#pragma once
#include <stdint.h>

// Immutable host-owned compiler inputs. A retained token keeps the existing
// cache entry and COM roots alive; it does not copy game GPU buffers or shaders.
struct GlassExperimentPipelineView
{
    uint32_t size, descriptorBytes;
    uint64_t identity;
    const void* descriptor; // D3D12_GRAPHICS_PIPELINE_STATE_DESC, token lifetime
    void* originalRoot;
    void* extendedRoot;
    uint32_t layout, dwords;
    uint32_t constantsSlot, previousSlot, currentSlot, materialSlot, captureSlot, instanceSlot;
    uint32_t rootParameterBytes, rootParameterCount;
    const void* rootParameters; // Owned D3D12_ROOT_PARAMETER1 array, token lifetime.
};
struct GlassExperimentPipelineAccess
{
    const void* source; // callback-scoped; only retain() may consume it
    void* (*retain)(const void* source);
    int32_t (*view)(const void* token, GlassExperimentPipelineView* output);
    void (*release)(void* token);
};
// Call retain only for a newly needed pipeline, not once per frame/draw. It
// allocates one small CPU token. Release on a module worker/control thread after
// all uses, including GPU/recording references. Never release under loader lock.
