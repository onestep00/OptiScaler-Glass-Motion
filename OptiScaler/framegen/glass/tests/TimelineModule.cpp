#include "../ExperimentTimelineModule.cpp"
#include <cstdio>
#include <vector>
#include <thread>
#include <barrier>
#include "../DiagnosticInstanceLookup.h"

GlassFg::DiagnosticInstanceLookup<4, 4> fixtureLookup;
bool packetScope = false;
int32_t fixturePacketQuery(uint64_t callsite, uint64_t mesh, uint32_t chunk, uint32_t instances,
    uint32_t ordinal, uint32_t first, uint32_t count, uint32_t transformIndex, uint32_t globalRange,
    GlassExperimentPacketParent* result)
{
    if (!packetScope || callsite != 0x1234 || mesh != 0x20000 || chunk != 7 || instances != 2 || ordinal ||
        first || count != 2 || transformIndex != 70 || globalRange != 1) return 0;
    result->proxy = 0x10000; result->mesh = mesh; result->entry = 0x40000; result->slot = 19;
    result->arrayCount = 40; result->count = count; result->transformIndex = transformIndex;
    result->globalRange = globalRange; return 1;
}
int32_t fixtureQuery(uint32_t frame, uint64_t mesh, uint32_t first, uint32_t count,
                     GlassExperimentInstanceSource* result)
{
    GlassFg::DiagnosticInstanceSource source;
    if (!fixtureLookup.query(frame, mesh, first, count, source)) return 0;
    result->proxy = source.proxy; result->mesh = source.mesh;
    result->renderer = source.renderer; result->scene = source.scene;
    result->frame = source.frame; result->ownerSlot = source.ownerSlot;
    result->originalCount = source.originalCount; result->selectedCount = source.count;
    result->linear = source.linear;
    std::copy(source.indices.begin(), source.indices.end(), result->indices);
    return 1;
}

struct TimelineFixture
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC descriptor {};
    std::array<GlassExperimentObject, 2> objects {{{0x10000, 0x20000, 7, 1, 0, 1, 50, 1},
                                                {0x30000, 0x20000, 8, 2, 1, 1, 70, 1}}};
    unsigned retains = 0, releases = 0;
};
void* fixtureRetain(const void* source)
{ auto* fixture = const_cast<TimelineFixture*>(static_cast<const TimelineFixture*>(source)); ++fixture->retains; return fixture; }
int32_t fixtureView(const void* token, GlassExperimentPipelineView* view)
{
    const auto* fixture = static_cast<const TimelineFixture*>(token);
    view->descriptor = &fixture->descriptor; view->descriptorBytes = sizeof(fixture->descriptor);
    return 1;
}
void fixtureRelease(void* token) { ++static_cast<TimelineFixture*>(token)->releases; }
int32_t fixtureObject(const void* source, uint32_t index, GlassExperimentObject* object)
{
    if (index >= 2) return 0;
    *object = static_cast<const TimelineFixture*>(source)->objects[index]; return 1;
}
template<class T> std::vector<T> readTimeline(const std::filesystem::path& path)
{
    const auto bytes = std::filesystem::file_size(path);
    if (bytes % sizeof(T)) throw std::runtime_error("Partial timeline record");
    std::vector<T> data(bytes / sizeof(T));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(data.data()), bytes);
    if (!file) throw std::runtime_error("Timeline read failed");
    return data;
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 1;
    const std::filesystem::path output = argv[1];
    TimelineFixture fixture;
    fixture.descriptor.NumRenderTargets = 1;
    auto& blend = fixture.descriptor.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE; blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_SRC1_COLOR; blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = 15;
    GlassExperimentCensusInput input {}; input.size = sizeof(input); input.operation = GlassCensusIndexed;
    auto& draw = input.draw; draw.size = sizeof(draw); draw.pipelineIdentity = 12;
    draw.source = &fixture; draw.objectAt = fixtureObject; draw.objectCount = 2;
    draw.pipelineAccess = {&fixture, fixtureRetain, fixtureView, fixtureRelease};
    GlassExperimentEvent event {sizeof(event), GlassExperimentCensus, 0, 0, 0,
                                GLASS_EXPERIMENT_CENSUS_VERSION, sizeof(input), &input};
    {
        TimelineRecorder<16, 30, 4> recorder(output);
        for (unsigned i = 0; i < 12; ++i)
        {
            event.frame = 100 + i; input.sequence = i + 1;
            fixture.objects[0].generation = i < 6 ? 1 : 3;
            if (recorder.observe(event) != 1) return 2;
        }
        // Unknown frame stays unknown; callback failure remains explicit.
        event.frame = 0; draw.objectCount = 3;
        if (recorder.observe(event) != 1) return 3;
        draw.objectCount = 2;
        while (recorder.observe(event) == 1) {}
        fixture.objects = {}; // Saved records must not depend on source storage.
        recorder.save();
        const auto rows = readTimeline<TimelineRow>(output / "draws.bin");
        const auto objects = readTimeline<GlassExperimentObject>(output / "objects.bin");
        if (rows.size() != 16 || objects.size() != 30 || rows[12].frame || !(rows[12].flags & 2) ||
            !(rows[15].flags & 4)) return 4;
        for (unsigned i = 0; i < 12; ++i)
        {
            if (rows[i].frame != 100 + i || rows[i].sequence != i + 1 || rows[i].objectCount != 2 || rows[i].flags ||
                objects[rows[i].objectFirst].generation != (i < 6 ? 1u : 3u) ||
                objects[rows[i].objectFirst + 1].transformIndex != 70) return 5;
        }
        if (fixture.retains != 1 || fixture.releases != 0) return 6;
    }
    if (fixture.releases != 1) return 7;
    TimelineFixture concurrent;
    concurrent.descriptor = fixture.descriptor;
    input.draw.source = &concurrent;
    input.draw.pipelineAccess.source = &concurrent;
    input.draw.objectCount = 2;
    {
        TimelineRecorder<8192, 16384, 4> recorder(output / "parallel");
        event.frame = 199; input.sequence = 0;
        if (recorder.observe(event) != 1) return 8; // Publish the immutable pipeline once.
        std::barrier ready(4);
        std::array<std::thread, 4> workers;
        std::atomic<unsigned> failures = 0;
        for (unsigned thread = 0; thread < workers.size(); ++thread)
            workers[thread] = std::thread([&, thread] {
                auto localInput = input;
                auto localEvent = event; localEvent.payload = &localInput;
                ready.arrive_and_wait();
                for (unsigned i = 0; i < 2000; ++i)
                {
                    localInput.sequence = 1 + thread * 2000 + i;
                    localEvent.frame = 200 + i;
                    if (recorder.observe(localEvent) != 1) ++failures;
                }
            });
        for (auto& worker : workers) worker.join();
        if (failures) return 9;
        recorder.save();
        const auto rows = readTimeline<TimelineRow>(output / "parallel/draws.bin");
        const auto objects = readTimeline<GlassExperimentObject>(output / "parallel/objects.bin");
        if (rows.size() != 8001 || objects.size() != 16002) return 10;
        std::vector<bool> sequences(8001), objectSlots(16002);
        for (const auto& row : rows)
        {
            if (row.sequence >= sequences.size() || sequences[row.sequence] || row.flags || row.objectCount != 2 ||
                row.objectFirst + 2 > objects.size()) return 11;
            sequences[row.sequence] = true;
            for (unsigned i = 0; i < 2; ++i)
            {
                const auto slot = row.objectFirst + i;
                if (objectSlots[slot] || objects[slot].proxy != concurrent.objects[i].proxy) return 12;
                objectSlots[slot] = true;
            }
        }
        if (concurrent.retains != 1 || concurrent.releases) return 13;
    }
    if (concurrent.releases != 1) return 14;
    {
        TimelineFixture linked;
        linked.descriptor = fixture.descriptor;
        linked.objects[0] = {0, 0, 0, 0, 0, 2, 70, 1};
        draw.source = &linked; draw.pipelineAccess.source = &linked;
        draw.mesh = 0x20000; draw.objectCount = 1; event.frame = 500;
        GlassFg::DiagnosticInstanceSource source;
        source.proxy = 0x10000; source.mesh = draw.mesh;
        source.renderer = 0x30000; source.scene = 0x40000;
        source.frame = 500; source.globalStart = 70; source.count = 2;
        source.originalCount = 40; source.ownerSlot = 19;
        source.indices[0] = 39; source.indices[1] = 4;
        if (!fixtureLookup.publish(source)) return 15;
        TimelineRecorder<16, 16, 4> recorder(output / "linked", fixtureQuery);
        if (recorder.observe(event) != 1) return 16;
        ++event.frame; // A previous frame's lookup is never accepted.
        if (recorder.observe(event) != 1) return 17;
        --event.frame; ++source.scene;
        if (fixtureLookup.publish(source) || recorder.observe(event) != 1) return 18;
        recorder.save();
        const auto saved = readTimeline<TimelineProvenance>(output / "linked/provenance.bin");
        const auto original = readTimeline<GlassExperimentObject>(output / "linked/objects.bin");
        if (saved.size() != 1 || saved[0].objectIndex != 0 || saved[0].source.indices[0] != 39 ||
            saved[0].source.indices[1] != 4 || saved[0].source.proxy != 0x10000 || original.size() != 3)
            return 19;
        for (const auto& object : original)
            if (object.proxy || object.generation || object.transformIndex != 70 || object.count != 2) return 20;
    }
    {
        TimelineFixture linked;
        linked.descriptor = fixture.descriptor;
        linked.objects[0] = {0, 0, 0, 0, 0, 2, 70, 1};
        draw.source = &linked; draw.pipelineAccess.source = &linked;
        draw.mesh = 0x20000; draw.chunk = 7; draw.instances = 2; draw.objectCount = 1;
        input.callsite = 0x1234; packetScope = true;
        TimelineRecorder<16, 16, 4> recorder(output / "packet", nullptr, fixturePacketQuery);
        if (recorder.observe(event) != 1) return 21;
        packetScope = false;
        if (recorder.observe(event) != 1) return 22;
        recorder.save();
        const auto saved = readTimeline<TimelineParent>(output / "packet/packet-parents.bin");
        if (saved.size() != 1 || saved[0].objectIndex || saved[0].source.proxy != 0x10000 ||
            saved[0].source.arrayCount != 40 || saved[0].source.slot != 19) return 23;
    }
    std::puts("PASS timeline_consecutive_frames=12 caller_data_copied=1 address_generation_preserved=1 "
              "bounded_capacity=1 unknown_frame_preserved=1 pipeline_retained_once=1 "
              "concurrent_draws=8000 concurrent_loss=0 object_slot_collisions=0 "
              "source_indices_preserved=1 stale_ambiguous_source_rejected=1 original_identity_unchanged=1 "
              "game_hooks=0 gpu_copies=0");
}
