#include "../GeometryObjectRegistry.h"
#include <atomic>
#include <cstdio>
#include <thread>

static void require(bool okay, const char* message)
{
    if (!okay)
        throw std::runtime_error(message);
}
static GlassFg::GeometryObjectPose pose(std::uint32_t x, std::uint32_t frame)
{
    GlassFg::GeometryObjectPose result;
    result.packed = { 0x3f800000, 0, 0, x, 0, 0x3f800000, 0, 0, 0, 0, 0x3f800000, 0 };
    result.bounds = { -1, -1, -1, 1, 1, 1 };
    result.frame = frame;
    return result;
}
int main()
{
    try
    {
        GlassFg::GeometryObjectRegistry registry(8);
        auto p = pose(0x7fc00000, 10); // Fixed-point position bits can spell float NaN.
        require(p.valid(), "Packed W lane was interpreted as a float");
        require(registry.registered(100, 2, 400, p), "Registration failed");
        auto original = registry.find(400, p.packed, 10);
        require(original && !registry.find(400, p.packed, 9), "Frame association");
        require(registry.registered(100, 2, 400, p) && registry.ticket(100, 2) == original.generation,
                "Duplicate registration replaced identity");
        require(registry.registered(200, 3, 400, p) && !registry.find(400, p.packed, 10),
                "Coincident live objects were guessed");
        registry.removed(200, 3);
        require(registry.find(400, p.packed, 10).proxy == 100, "Removal left an ambiguous candidate");
        registry.removed(100, 2);
        require(!registry.find(400, p.packed, 10), "Dead object remained findable");
        require(registry.registered(100, 2, 400, p), "Pointer reuse registration");
        const auto generation = registry.ticket(100, 2);
        require(generation != original.generation && !registry.update(100, 2, original.generation, 400, pose(1, 11)),
                "Stale callback changed a reused identity");
        for (unsigned i = 1; i <= 8; ++i)
            require(registry.update(100, 2, generation, 400, pose(i, 10 + i)), "Pose update");
        require(!registry.find(400, pose(4, 14).packed, 20), "Bounded history did not evict oldest pose");
        for (unsigned i = 5; i <= 8; ++i)
            require(registry.find(400, pose(i, 10 + i).packed, 20).generation == generation, "Recent upload pose lost");
        auto invalid = pose(9, 19);
        invalid.packed[0] = 0x7fc00000;
        require(!registry.update(100, 2, generation, 400, invalid), "Nonfinite linear transform accepted");
        require(registry.update(100, 2, generation, 500, pose(9, 19)), "Mesh replacement");
        require(registry.ticket(100, 2) != generation && !registry.find(400, pose(8, 18).packed, 20),
                "Mesh replacement inherited old geometry history");

        GlassFg::GeometryObjectRegistry concurrent(64);
        std::atomic<unsigned> errors = 0;
        std::vector<std::thread> workers;
        for (unsigned index = 0; index < 32; ++index)
            workers.emplace_back(
                [&, index]
                {
                    const auto object = std::uint64_t(1000 + index);
                    if (!concurrent.registered(object, index, 900, pose(index * 1000, 0)))
                    {
                        ++errors;
                        return;
                    }
                    const auto ticket = concurrent.ticket(object, index);
                    for (unsigned frame = 1; frame <= 128; ++frame)
                    {
                        const auto current = pose(index * 1000 + frame, frame);
                        if (!concurrent.update(object, index, ticket, 900, current) ||
                            concurrent.find(900, current.packed, frame).proxy != object)
                            ++errors;
                    }
                    concurrent.removed(object, index);
                });
        for (auto& worker : workers)
            worker.join();
        require(!errors && concurrent.stats().live == 0 && concurrent.stats().updates == 4096,
                "Concurrent event/query mismatch");
        printf("PASS engine_slot_generation=1 pointer_reuse=1 duplicate_registration=1 ambiguous_objects_rejected=1 "
               "mesh_replacement=1 recent_pose_lookup=1 fixed_point_bits=1 concurrent_updates=4096\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
