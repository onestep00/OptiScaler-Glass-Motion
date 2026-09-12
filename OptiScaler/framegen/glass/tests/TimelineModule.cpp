#include "../ExperimentTimelineModule.cpp"
#include <cstdio>
#include <vector>
#include <thread>
#include <barrier>

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
    std::puts("PASS timeline_consecutive_frames=12 caller_data_copied=1 address_generation_preserved=1 "
              "bounded_capacity=1 unknown_frame_preserved=1 pipeline_retained_once=1 "
              "concurrent_draws=8000 concurrent_loss=0 object_slot_collisions=0 game_hooks=0 gpu_copies=0");
}
