#pragma once
#include "ExperimentDrawAbi.h"
#include <sstream>
#include <stdexcept>
#include <string>

namespace GlassFg
{
// Optional diagnostic filter from a census in the same running process. This
// selects original draws only; it supplies no missing identity/geometry/history.
// UINT32_MAX startInstance permits changing engine upload offsets during capture.
struct ExperimentCaptureSelection
{
    bool enabled = false;
    uint64_t pipeline = 0, target = 0, mesh = 0, proxy = 0;
    uint32_t binding = 0, chunk = 0, indices = 0, instances = 0, startIndex = 0, startInstance = 0;
    int32_t baseVertex = 0;
    static ExperimentCaptureSelection parse(const std::string& line, uint32_t process)
    {
        ExperimentCaptureSelection result;
        if (line.empty()) return result;
        std::istringstream input(line); std::string format; uint32_t pid = 0;
        if (!(input >> format >> pid >> result.pipeline >> result.binding >> result.target >> result.mesh >> result.chunk >>
              result.indices >> result.instances >> result.startIndex >> result.baseVertex >> result.startInstance >> result.proxy) ||
            format != "select-v1" || pid != process || !result.pipeline || result.binding > 8 || !result.target ||
            !result.mesh || !result.indices || !result.instances || result.instances > 64)
            throw std::runtime_error("Invalid or foreign-process capture selector");
        input >> std::ws;
        if (!input.eof()) throw std::runtime_error("Unexpected capture selector fields");
        result.enabled = true; return result;
    }
    bool matches(const GlassExperimentDrawInput& draw) const
    {
        if (!enabled) return true;
        if (draw.pipelineIdentity != pipeline || draw.mesh != mesh || draw.chunk != chunk || draw.indices != indices ||
            draw.instances != instances || draw.startIndex != startIndex || draw.baseVertex != baseVertex ||
            (startInstance != UINT32_MAX && draw.startInstance != startInstance) || !draw.targetAt) return false;
        GlassExperimentTarget view {}; view.size = sizeof(view);
        if (draw.targetAt(draw.targetSource, binding, &view) != 1 || view.resource != target) return false;
        if (!proxy) return true;
        if (!draw.objectAt || draw.objectCount > 4096) return false;
        for (unsigned i = 0; i < draw.objectCount; ++i)
        {
            GlassExperimentObject object {};
            if (draw.objectAt(draw.source, i, &object) == 1 && object.proxy == proxy && object.mesh == mesh && object.generation)
                return true;
        }
        return false;
    }
};
} // namespace GlassFg
