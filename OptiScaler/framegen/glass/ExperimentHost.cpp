#include "pch.h"
#include "ExperimentHost.h"
#include "ExperimentControl.h"
#include "NativeHost.h"
#include <thread>

namespace GlassFg
{
bool StartExperimentHost(ID3D12Device* device, const std::filesystem::path& directory) noexcept
{
    bool requested = false;
    try
    {
        requested = std::filesystem::is_regular_file(directory / "experiment-host.enable");
        if (!requested) return false;
        Microsoft::WRL::ComPtr<ID3D12Device> retained = device;
        std::thread([retained, directory]
        {
            try
            {
                auto* control = new ExperimentControl(retained.Get(), directory, NativeCaptureSubmissionReady);
                control->run(); // Process-resident, including registered callbacks.
            }
            catch (const std::exception& error)
            {
                std::ofstream(directory / "experiment-host.error", std::ios::app) << error.what() << '\n';
            }
        }).detach();
    }
    catch (...)
    {
        if (requested)
            try { std::ofstream(directory / "experiment-host.error", std::ios::app) << "Host thread startup failed\n"; }
            catch (...) {}
    }
    return requested;
}
}
