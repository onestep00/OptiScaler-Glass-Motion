#include "../PackedMotionMappings.h"
#include <cstdio>
#include <stdexcept>
using namespace GlassFg;
void require(bool value) { if (!value) throw std::runtime_error("Packed mapping ownership failed"); }
int main()
{
    try
    {
        PackedMotionMappings<1, 8, 16, 4> maps;
        VertexHistoryKey object {{0x1000,0x2000,1,7},3,4,5,0,1};
        auto chunk = object; chunk.chunk = 1; chunk.pipeline = 8;
        auto element = object; element.arrayGeneration = 1; element.sourceIndex = 4;
        auto second = element; second.sourceIndex = 5;
        require(maps.beginFrame(10,0));
        const auto a = maps.acquire(object,8,10), b = maps.acquire(chunk,8,10);
        const auto c = maps.acquire(element,8,10), d = maps.acquire(second,8,10);
        require(a && b && c && d && a.history.base != b.history.base && a.boundaryId == b.boundaryId);
        require(c.boundaryId != d.boundaryId && a.boundaryId != c.boundaryId);
        require(c.history.base != d.history.base && a.history.generation != b.history.generation);
        require(maps.beginFrame(11,10));
        // Draw order changes; history remains with the original object/element.
        require(maps.acquire(second,8,11).history.generation == d.history.generation);
        require(maps.acquire(chunk,8,11).history.generation == b.history.generation);
        require(maps.acquire(object,8,11).boundaryId == maps.acquire(chunk,8,11).boundaryId);
        require(!maps.acquire(object,9,11));
        auto newLife = object; ++newLife.object.generation;
        const auto e = maps.acquire(newLife,8,11);
        require(e && e.boundaryId != maps.acquire(object,8,11).boundaryId && e.history.generation != a.history.generation);
        require(maps.beginFrame(14,11) && maps.reservedVertices() == 0);
        const auto reused = maps.acquire(object,8,14);
        require(reused && reused.history.generation != a.history.generation);
        require(!maps.acquire(object,8,13) && !maps.beginFrame(13,11));
        std::puts("PASS chunk_history_separate=1 shared_object_boundary=1 array_elements_separate=1 "
                  "reorder_stable=1 retired_storage_reclaimed=1 reused_gpu_generation_changed=1 gpu_work=0");
    }
    catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
