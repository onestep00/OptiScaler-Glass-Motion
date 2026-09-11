#include "../ExperimentAbi.h"
#include "ExperimentGpuPayload.h"
#include <wrl/client.h>
#include <memory>
#ifndef FIXTURE_ID
#define FIXTURE_ID 1
#endif
namespace
{
struct Context { Microsoft::WRL::ComPtr<ID3D12Resource> upload; };
int32_t create(const GlassExperimentHost* host, void** context)
{
    *context = nullptr;
    auto owned = std::make_unique<Context>();
    auto* device = static_cast<ID3D12Device*>(host->device);
    if (!device) return -1;
    D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&owned->upload)))) return -1;
    void* bytes = nullptr; D3D12_RANGE noRead { 0, 0 };
    if (FAILED(owned->upload->Map(0, &noRead, &bytes))) return -1;
    for (unsigned i = 0; i < 64; ++i) static_cast<unsigned*>(bytes)[i] = FIXTURE_ID;
    owned->upload->Unmap(0, nullptr);
    *context = owned.release();
    return 0;
}
int32_t event(void* context, const GlassExperimentEvent* event)
{
    if (event->payloadVersion != 1 || event->payloadBytes != sizeof(ExperimentGpuPayload)) return -1;
    const auto& args = *static_cast<const ExperimentGpuPayload*>(event->payload);
    args.command->CopyBufferRegion(args.destination, args.offset, static_cast<Context*>(context)->upload.Get(), 0, 256);
    return FIXTURE_ID;
}
void destroy(void* context) { delete static_cast<Context*>(context); }
const GlassExperimentApi api { sizeof(api), GLASS_EXPERIMENT_ABI, GlassExperimentFg, create, event, destroy };
}
extern "C" __declspec(dllexport) const GlassExperimentApi* GlassExperimentQuery() { return &api; }
