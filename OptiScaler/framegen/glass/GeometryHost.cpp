#include "pch.h"
#include "GeometryCreation.h"
#include "CyberpunkSurfacePass.h"
#include "CyberpunkObjects.h"
#include "CyberpunkDraws.h"
#include <Util.h>
#include <mutex>

namespace GlassFg
{
void InitializeGeometryHost(ID3D12Device* device) noexcept
{
    try
    {
        static std::once_flag once;
        std::call_once(once,
                       [device]
                       {
                           CyberpunkSurfacePass executable;
                           if (!device || !executable.initialize(GetModuleHandleW(nullptr), nullptr))
                               return;
                           const auto directory = Util::DllPath().parent_path();
                           const auto compiler = directory / L"Glass" / L"dxcompiler.dll";
                           const bool files = std::filesystem::is_regular_file(compiler) &&
                                              std::filesystem::is_regular_file(directory / L"Glass" / L"dxil.dll");
                           const bool ready = files && StartGeometryCreation(device, compiler);
                           const bool objects = ready && InitializeCyberpunkObjects(GetModuleHandleW(nullptr));
                           const bool draws = objects && InitializeCyberpunkDraws(GetModuleHandleW(nullptr));
                           if (FILE* log = _wfopen((directory / L"OptiScaler.Glass.Geometry.log").c_str(), L"a"))
                           {
                               fprintf(log,
                                       "GEOMETRY_CREATION active=%u compiler_files=%u mesh_registry=%u draw_packets=%u "
                                       "actual_draw_replacement=0 fg_substitution=0\n",
                                       ready, files, objects, draws);
                               fclose(log);
                           }
                       });
    }
    catch (...)
    {
    } // Optional acquisition never changes the application's result.
}
} // namespace GlassFg
