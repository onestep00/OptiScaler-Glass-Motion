#include "../ExperimentCaptureSelection.h"
#include <iostream>

void require(bool v) { if (!v) throw std::runtime_error("Capture selector failed"); }
int main()
{
    using S=GlassFg::ExperimentCaptureSelection;
    auto s=S::parse("select-mesh-v1 42 0 70 80",42);
    GlassExperimentDrawInput d{}; d.mesh=80;
    d.targetAt=[](const void*,uint32_t slot,GlassExperimentTarget* result)->int32_t {
        if(slot) return 0; result->resource=70; return 1;
    };
    require(s.meshOnly && s.matches(d));
    d.chunk=9; d.instances=40; d.pipelineIdentity=999; d.startInstance=1234;
    require(s.matches(d));
    d.mesh=81; require(!s.matches(d)); d.mesh=80;
    auto wrong=s; wrong.target=71; require(!wrong.matches(d));
    for(const auto* text : {"select-mesh-v1 43 0 70 80", "select-mesh-v1 42 9 70 80",
        "select-mesh-v1 42 0 0 80", "select-mesh-v1 42 0 70 80 extra"}) {
        bool failed=false; try { S::parse(text,42); } catch(const std::runtime_error&) { failed=true; }
        require(failed);
    }
    auto exact=S::parse("select-v1 42 999 0 70 80 9 12 40 0 0 4294967295 0",42);
    d.indices=12; require(exact.matches(d)); ++d.chunk; require(!exact.matches(d));
    std::cout<<"PASS mesh_chunks=1 target_scope=1 foreign_pid_rejected=1 exact_selector_preserved=1\n";
}
