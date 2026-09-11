#define GLASS_NODE_LIFETIME
#include "../ExperimentInstanceUpdates.cpp"
#include <iostream>
#include <stdexcept>

std::array<std::uint64_t, 32> instance {}, node {}, shared {}, fixtureProxy {};
std::array<std::uint64_t, 64> mesh {};
std::array<std::uint64_t, 2> handles {}, span {0x40000 + 107 * 48, 0x40000 + 120 * 48};
std::array<std::uint64_t, 3> fixtureHandle {};
unsigned forwarded = 0, released = 0;
void require(bool value) { if (!value) throw std::runtime_error("Node lifetime observation failed"); }
std::uint64_t address(const auto& value) { return reinterpret_cast<std::uint64_t>(value.data()); }
void forward(void* a, float distance, void* b, void* bounds)
{
    require(a == instance.data() && distance == 3.25f && b == span.data() && !bounds);
    handles[0] = address(fixtureHandle);
    instance[0xf0 / 8] = 1 | (std::uint64_t(1) << 32);
    ++forwarded;
}
void release(void* value)
{
    require(value == fixtureHandle.data());
    require(!sourceOwners.find(address(fixtureProxy), 0x123456)); // cancelled BEFORE engine release
    fixtureHandle[2] = 0;
    ++released;
}
int main()
{
    original = forward; originalDestroy = release;
    instance[0x60 / 8] = address(node); instance[0xb8 / 8] = address(mesh);
    instance[0xe8 / 8] = address(handles);
    node[0x38 / 8] = address(shared); node[0x48 / 8] = 100 | (std::uint64_t(40) << 32);
    shared[0x30 / 8] = 0x40000; // Unmapped transform contents are never read.
    shared[0x38 / 8] = 200 * 48;
    mesh[0x1f0 / 8] = fixtureProxy[0xd8 / 8] = 0x123456;
    auto create = [&] {
        fixtureHandle[2] = address(fixtureProxy); instance[0xf0 / 8] = 1;
        enqueue(instance.data(), 3.25f, span.data(), nullptr);
    };
    recording = true; create();
    auto first = sourceOwners.find(address(fixtureProxy), 0x123456);
    require(first && first.source.first == 107 && first.source.count == 13);
    require(used == 1 && rows[0].header[15] == 0x123456 && rows[0].header[17] == first.generation);
    GlassExperimentSourceQuery query = &GlassSourceOwnerQuery;
    GlassExperimentSourceOwner result;
    require(query(address(fixtureProxy), 0x123456, 13, &result) == 0 && !result.generation);
    sourceReady = true;
    require(query(address(fixtureProxy), 0x123456, 13, &result) == 1 &&
            result.generation == first.generation && result.first == 107 && result.count == 13 &&
            result.node == address(instance) && result.buffer == address(shared));
    require(query(address(fixtureProxy), 0x123456, 12, &result) == 0 && !result.node);
    result.version = 2;
    require(query(address(fixtureProxy), 0x123456, 13, &result) == -1);
    result = {};
    destroy(fixtureHandle.data());
    require(sourceOwners.size() == 0 && released == 1);
    require(query(address(fixtureProxy), 0x123456, 13, &result) == 0 && !result.generation);
    recording = false; create();
    auto next = sourceOwners.find(address(fixtureProxy), 0x123456);
    require(next && next.generation > first.generation && used == 1);
    mesh[0x1f0 / 8] = 0x999999; create();
    require(!sourceOwners.find(address(fixtureProxy), 0x123456) && sourceRejected == 1);
    destroy(fixtureHandle.data());
    require(sourceOwners.size() == 0 && forwarded == 3 && released == 2 && active == 0);
    mesh[0x1f0 / 8] = 0x123456; create();
    require(query(address(fixtureProxy), 0x123456, 13, &result) == 1);
    fixtureProxy[0x108 / 8] = 0x60000; fixtureProxy[0x110 / 8] = 13;
    GlassFg::CyberpunkSourceCandidate candidate {address(instance), address(node), 0, address(shared), 0,
        address(fixtureHandle), address(fixtureProxy), address(mesh), 107, 13, 0, 0};
    sourceOwners.destroyed(address(fixtureHandle), address(fixtureProxy));
    { LifecycleScope inFlight; require(!seedSource(candidate)); }
    bool interrupted = false;
    require(!seedSource(candidate, [&](std::uint64_t where, void* destination, unsigned bytes) {
        if (!interrupted) { interrupted = true; LifecycleScope event; }
        return read(where, destination, bytes);
    }));
    require(seedSource(candidate) && sourceSeeded == 1);
    const auto seededGeneration = sourceOwners.find(address(fixtureProxy), 0x123456).generation;
    require(seedSource(candidate) && sourceSeeded == 1 &&
            sourceOwners.find(address(fixtureProxy), 0x123456).generation == seededGeneration);
    fixtureProxy[0x108 / 8] = 0;
    require(seedSource(candidate) && sourceSeeded == 1 &&
            sourceOwners.find(address(fixtureProxy), 0x123456).generation == seededGeneration);
    ++candidate.nodeControl; require(!seedSource(candidate)); --candidate.nodeControl;
    originalDestroy = [](void*) {};
    destroy(reinterpret_cast<void*>(1));
    require(!sourceHealthy && query(address(fixtureProxy), 0x123456, 13, &result) == 0 && !result.generation);
    require(profileMagic == 0x49555035);
    std::cout << "PASS source_publication=1 cancellation_before_release=1 same_address_reuse=1 "
                 "stopped_csv_tracking=1 typed_mesh_rejection=1 transform_copies=0 "
                 "query_abi=1 query_after_destruction_rejected=1 missed_lifecycle_disables_query=1 "
                 "bootstrap_current_chain=1 bootstrap_inflight_rejected=1 bootstrap_epoch_rejected=1 null_transient_pointer=1 "
                 "live_engine=0 object_motion_produced=0\n";
}
