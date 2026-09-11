#include "../ExperimentDrawAbi.h"
#include <d3d12.h>
#include <new>

// Own-device GeometryCommandFixture contract only. Never load into the game.
namespace
{
int32_t create(const GlassExperimentHost*, void** context)
{
    *context = new (std::nothrow) unsigned(0);
    return *context ? 0 : -1;
}
int32_t event(void* context, const GlassExperimentEvent* value)
{
    if (!value || value->kind != GlassExperimentDraw || value->payloadVersion != 1 ||
        value->payloadBytes != sizeof(GlassExperimentDrawInput) || !value->payload ||
        value->frame != 42 || value->phase || value->phaseCount) return -10;
    const auto& d = *static_cast<const GlassExperimentDrawInput*>(value->payload);
    if (d.size != sizeof(d) || !d.command || !d.recording || !d.pipelineIdentity || d.mesh != 2 ||
        !d.originalPipeline || !d.originalRoot || !d.descriptor ||
        d.descriptorBytes != sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC) ||
        d.chunk || d.indices != 6 || d.startIndex || d.baseVertex != 2 ||
        d.startInstance < 7 || d.startInstance > 9 || !d.rootReplayable || !d.rasterKnown ||
        d.viewport[2] != 160 || d.viewport[3] != 112 || d.scissor[2] != 160 || d.scissor[3] != 112 ||
        d.renderTargetCount != 1 || !d.renderTargets[0] || !d.depthTarget ||
        !d.objectAt || !d.meshShape || !d.source || d.objectCount != d.instances ||
        (d.instances != 1 && d.instances != 3)) return -11;
    const auto& p = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(d.descriptor);
    if (p.pRootSignature != d.originalRoot || !p.VS.pShaderBytecode || !p.VS.BytecodeLength ||
        !p.PS.pShaderBytecode || !p.PS.BytecodeLength) return -12;
    GlassExperimentObject object {};
    const uint64_t proxies[] { 1, 5, 8 };
    const uint32_t slots[] { 3, 6, 9 }, generations[] { 4, 7, 10 };
    for (uint32_t i = 0; i < d.objectCount; ++i)
        if (!d.objectAt(d.source, i, &object) || object.proxy != proxies[i] || object.mesh != 2 ||
            object.slot != slots[i] || object.generation != generations[i] || object.first != i ||
            object.count != 1 || object.transformIndex != i || object.globalRange) return -13;
    if (d.objectAt(d.source, d.objectCount, &object) || d.objectAt(nullptr, 0, &object) ||
        d.objectAt(d.source, 0, nullptr)) return -14;
    GlassExperimentMesh mesh {};
    mesh.chunkAddress = 123;
    if (d.meshShape(d.source, &mesh) || mesh.chunkAddress || mesh.vertices || mesh.indices)
        return -15; // This fixture has no real engine mesh allocation.
    ++*static_cast<unsigned*>(context);
    return static_cast<int32_t>(d.objectCount);
}
void destroy(void* context) { delete static_cast<unsigned*>(context); }
const GlassExperimentApi api { sizeof(api), GLASS_EXPERIMENT_ABI, GlassExperimentDraw, create, event, destroy };
}
extern "C" __declspec(dllexport) const GlassExperimentApi* GlassExperimentQuery() { return &api; }
