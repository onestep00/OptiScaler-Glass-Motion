#include "pch.h"
#include "../CommandLifetime.h"
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void check(bool value)
{
    if (!value) throw std::runtime_error("command lifetime contract failed");
}
int main()
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    check(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))));
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator))));
    auto create = [&] {
        check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(), nullptr,
                                                IID_PPV_ARGS(&command))));
        check(SUCCEEDED(command->Close()));
    };
    create();
    GlassFg::CommandLifetime life;
    check(life.attach(command.Get()) && !life.attach(command.Get()) && !life.takeDestroyed());
    auto* identity = command.Get();
    command.Reset();
    check(life.takeDestroyed() == identity && !life.takeDestroyed());
    create();
    check(life.attach(command.Get()) && life.detachLive(command.Get()) && !life.identity());
    command.Reset();
    check(!life.takeDestroyed());
    create();
    {
        GlassFg::CommandLifetime shortLived;
        check(shortLived.attach(command.Get()));
        // The callback state outlives the watch/session, without referencing it.
    }
    command.Reset();
    std::puts("COMMAND_LIFETIME notifier=pass destruction=pass live_unregister=pass owner_early_exit=pass");
}
