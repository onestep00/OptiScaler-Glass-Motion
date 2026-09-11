// Production callbacks on owned renderer layouts. No game hooks are installed.
#include "../CyberpunkDraws.cpp"
#include <cstdio>
#include <vector>

namespace GlassFg
{
std::shared_ptr<GeometryObjectRegistry> GetCyberpunkObjects() noexcept { return {}; }
} // namespace GlassFg
namespace
{
using namespace GlassFg;
std::unique_ptr<EngineDrawState> fixture;
std::uint32_t tick = 77, origin = 320;
std::vector<std::byte> rendererMemory(0xb80000), rootMemory(0x4630), gpuUpload(65536);
std::uint64_t root = reinterpret_cast<std::uint64_t>(rootMemory.data());
std::array<std::array<std::byte, 0x1c0>, 3> proxies {};
std::array<std::byte, 512> rigidBytes {}, skinnedBytes {}, target {};
std::array<std::byte, 0xb0> meshBytes {};
std::array<std::byte, 8 * 0xf8> chunkBytes {};
std::array<std::uint64_t, 2> packet {};
EngineGeometry geometry;
EngineContext context;
GeometryObjectPose pose;
std::vector<GeometryBatchSpan> observed;
GeometryDrawView observedHeader;
bool crossFrame = false, badUpload = false;
unsigned forwardedAppends = 0, forwardedFlushes = 0;
const void* callsite = reinterpret_cast<void*>(0x12345678);

void require(bool okay, const char* text)
{
    if (!okay)
        throw std::runtime_error(text);
}
template <typename T> void put(void* address, std::size_t offset, const T& value)
{
    memcpy(static_cast<std::byte*>(address) + offset, &value, sizeof(value));
}
void storeInstance(unsigned object, unsigned transform, bool skin, unsigned count = 1, bool global = false)
{
    const auto renderer = reinterpret_cast<std::uint64_t>(rendererMemory.data());
    const auto proxy = reinterpret_cast<std::uint64_t>(proxies[object].data());
    const std::uint32_t slot = object + 1;
    put(proxies[object].data(), 0x98, slot);
    put(proxies[object].data(), 0xd8, geometry.mesh);
    const auto encoded = proxy | (0xa5ull << 56);
    put(rendererMemory.data(), 0x274248 + slot * 24, encoded);
    pose.packed = { 0x3f800000, 0, 0, 0, 0, 0x3f800000, 0, 0, 0, 0, 0x3f800000, 0 };
    pose.bounds = { -1, -1, -1, 1, 1, 1 };
    pose.frame = tick;
    fixture->registry->registered(proxy, slot, geometry.mesh, pose);
    for (unsigned i = 0; i < count; ++i)
        memcpy(rendererMemory.data() + 0x574280 + (transform + i) * 48, pose.packed.data(), 48);
    context.entry = renderer + 0x274248 + slot * 24;
    packet = { global ? 1ull << 59 : 0, slot | (std::uint64_t(count) << 18) | (std::uint64_t(transform) << 33) |
                                            (1ull << 51) | (skin ? 1ull << 50 : 0) };
    GlassFg::append(reinterpret_cast<void*>(renderer + 0x574280 + transform * 48), packet.data(), 35, 37, &context);
}
void fixtureAppend(void* transforms, void* packetPointer, std::uintptr_t c, std::uintptr_t d, void* ctx)
{
    require(c == 35 && d == 37 && ctx == &context && packetPointer == packet.data(), "Append ABI forwarding");
    const bool skin = (packet[1] & (1ull << 50)) != 0;
    const auto count = static_cast<unsigned>((packet[1] >> 18) & 0x7fff);
    auto& used = skin ? context.skinCount : context.rigidCount;
    const auto stride = skin ? 64 : 48;
    auto* data = reinterpret_cast<std::byte*>(skin ? context.skinned : context.rigid);
    for (unsigned i = 0; i < count; ++i)
        memcpy(data + (used + i) * stride, static_cast<std::byte*>(transforms) + i * 48, 48);
    used += count;
    ++forwardedAppends;
    if (crossFrame)
        ++tick;
}
std::uint32_t fixtureUpload(void* destination, void* source, std::uint32_t count, void*)
{
    require(destination == target.data(), "Upload target forwarding");
    std::uint32_t stride = 0;
    copyAt(reinterpret_cast<std::uint64_t>(destination) + 0x14, stride);
    if (badUpload)
        return UINT32_MAX;
    memcpy(gpuUpload.data() + origin * stride, source, count * stride);
    return origin;
}
void draw(std::uint32_t count, std::uint32_t start)
{
    require(ReadCyberpunkGeometryDraw(nullptr, geometry.indexCount, count, 0, 0, start).objects.empty(),
            "Unknown draw call site admitted");
    require(ReadCyberpunkGeometryDraw(callsite, geometry.indexCount, count, 0, 0, start + 1).objects.empty(),
            "Wrong instance offset admitted");
    auto value = ReadCyberpunkGeometryDraw(callsite, geometry.indexCount, count, 0, 0, start);
    if (!value.objects.empty())
    {
        const auto shape = ReadCyberpunkMeshShape(value);
        require(shape && shape.vertices == 24 && shape.indices == 60 && shape.streams == 2 &&
                    shape.vertexBuffer == 12 && shape.indexBuffer == 13 && shape.indexOffset == 128 &&
                    shape.streamOffsets[0] == 256 && shape.streamOffsets[1] == 2048,
                "Actual mesh chunk allocation range mismatch");
        put(chunkBytes.data(), 7 * 0xf8 + 0xec, std::uint16_t(0));
        require(!ReadCyberpunkMeshShape(value), "Empty vertex allocation admitted");
        put(chunkBytes.data(), 7 * 0xf8 + 0xec, std::uint16_t(24));
        put(chunkBytes.data(), 7 * 0xf8 + 0xe8, std::uint32_t(59));
        require(!ReadCyberpunkMeshShape(value), "Unrelated index count admitted");
        put(chunkBytes.data(), 7 * 0xf8 + 0xe8, std::uint32_t(60));
        put(meshBytes.data(), 0xac, std::uint32_t(7));
        require(!ReadCyberpunkMeshShape(value), "Outside chunk array admitted");
        put(meshBytes.data(), 0xac, std::uint32_t(8));
    }
    observed.assign(value.objects.begin(), value.objects.end());
    observedHeader = value;
    require(ReadCyberpunkGeometryDraw(callsite, geometry.indexCount, count, 0, 0, start).objects.empty(),
            "Draw mapping consumed twice");
}
void fixtureRigid(void* a, void* desc, void* ctx, std::uint32_t global, bool half)
{
    require(a == target.data() && desc == &geometry && ctx == &context && !half, "Rigid ABI forwarding");
    put(target.data(), 0x14, std::uint32_t(48));
    const auto start =
        global == UINT32_MAX ? GlassFg::upload(target.data(), rigidBytes.data(), context.rigidCount, ctx) : global;
    draw(context.rigidCount, start);
    ++forwardedFlushes;
}
void fixtureSkinned(void* a, void* desc, void* ctx, bool half)
{
    require(a == target.data() && desc == &geometry && ctx == &context && !half, "Skinned ABI forwarding");
    put(target.data(), 0x14, std::uint32_t(64));
    const auto start = GlassFg::upload(target.data(), skinnedBytes.data(), context.skinCount, ctx);
    draw(context.skinCount, start);
    ++forwardedFlushes;
}
void flush(bool skin = false, std::uint32_t global = UINT32_MAX)
{
    observed.clear();
    if (skin)
    {
        GlassFg::skinned(target.data(), &geometry, &context, false);
        context.skinCount = 0;
    }
    else
    {
        GlassFg::rigid(target.data(), &geometry, &context, global, false);
        context.rigidCount = 0;
    }
    require(ReadCyberpunkGeometryDraw(callsite, geometry.indexCount, 1, 0, 0, origin).objects.empty(),
            "Mapping escaped the flush callback");
    require(!ReadCyberpunkMeshShape(observedHeader), "Chunk read escaped draw scope");
}
void fixtureRun(void*, void*, void*)
{
    storeInstance(0, 10, false);
    storeInstance(1, 11, false);
    flush();
    require(observed.size() == 2 && observed[0].identity.slot == 1 && observed[1].identity.slot == 2,
            "Coincident proxies collapsed");
    require(observedHeader.stride == 48 && observedHeader.startInstanceLocation == origin && observedHeader.chunk == 7,
            "Rigid mesh/chunk/stride provenance");
    const auto generation = observed[0].identity.generation;
    storeInstance(1, 12, false);
    storeInstance(0, 13, false);
    flush();
    require(observed.size() == 2 && observed[0].identity.slot == 2 && observed[1].identity.generation == generation,
            "Batch reordering changed identities");
    storeInstance(0, 14, true);
    storeInstance(1, 15, true);
    flush(true);
    require(observed.size() == 2 && observedHeader.stride == 64 && observed[1].identity.slot == 2,
            "Skinned provenance");
    storeInstance(0, 16, false);
    storeInstance(1, 17, false, 2);
    storeInstance(2, 19, false);
    flush();
    require(observed.size() == 3 && !observed[1].identity && observed[1].count == 2 && observed[2].first == 3 &&
                observed[2].identity.slot == 3,
            "Unknown internal instances shifted later object IDs");
    storeInstance(0, 21, false, 1, true);
    storeInstance(1, 22, false, 1, true);
    flush(false, 21);
    require(observed.size() == 2 && observedHeader.startInstanceLocation == 21, "Global transform range");
    storeInstance(0, 24, false, 1, true);
    storeInstance(1, 26, false, 1, true);
    flush(false, 24);
    require(observed.empty(), "Noncontiguous global range admitted");
    badUpload = true;
    storeInstance(0, 28, false);
    flush();
    require(observed.empty(), "Failed engine upload admitted");
    badUpload = false;
    storeInstance(0, 29, false);
    const auto proxy = reinterpret_cast<std::uint64_t>(proxies[0].data());
    fixture->registry->removed(proxy, 1);
    fixture->registry->registered(proxy, 1, geometry.mesh, pose);
    flush();
    require(observed.empty(), "Reused slot consumed old packet identity");
    put(proxies[0].data(), 0x108, std::uint64_t(0x12340000));
    put(proxies[0].data(), 0x110, std::uint32_t(40));
    storeInstance(0, 31, false);
    flush();
    require(observed.size() == 1 && !observed[0].identity,
            "One visible instance of a CPU cluster acquired proxy-only history");
    put(proxies[0].data(), 0x108, std::uint64_t(0));
    put(proxies[0].data(), 0x114, std::uint32_t(300));
    storeInstance(0, 32, false);
    flush();
    require(observed.size() == 1 && !observed[0].identity,
            "One visible instance of a global cluster acquired proxy-only history");
    put(proxies[0].data(), 0x110, std::uint32_t(0));
    put(proxies[0].data(), 0x114, UINT32_MAX);
    storeInstance(0, 33, false);
    flush();
    require(observed.size() == 1 && observed[0].identity.slot == 1,
            "Ordinary proxy admission did not recover");
    crossFrame = true;
    storeInstance(0, 30, false);
    flush();
    require(observed.empty(), "Mixed frame admitted");
    crossFrame = false;
}
} // namespace
int main()
{
    try
    {
        fixture = std::make_unique<GlassFg::EngineDrawState>();
        fixture->registry = std::make_shared<GlassFg::GeometryObjectRegistry>(8);
        fixture->tick = &tick;
        fixture->rendererGlobal = &root;
        fixture->drawReturn = callsite;
        for (auto& proxy : proxies) put(proxy.data(), 0x114, UINT32_MAX);
        const auto renderer = reinterpret_cast<std::uint64_t>(rendererMemory.data());
        put(rootMemory.data(), 0x4628, renderer);
        geometry.kind = 0;
        geometry.mesh = reinterpret_cast<std::uint64_t>(meshBytes.data());
        put(meshBytes.data(), 0x30, std::uint32_t(12));
        put(meshBytes.data(), 0x34, std::uint32_t(13));
        put(meshBytes.data(), 0xa0, reinterpret_cast<std::uint64_t>(chunkBytes.data()));
        put(meshBytes.data(), 0xa8, std::uint32_t(8));
        put(meshBytes.data(), 0xac, std::uint32_t(8));
        put(chunkBytes.data(), 7 * 0xf8 + 0xb8, std::uint32_t(256));
        put(chunkBytes.data(), 7 * 0xf8 + 0xbc, std::uint32_t(2048));
        put(chunkBytes.data(), 7 * 0xf8 + 0xcc, std::uint32_t(2));
        put(chunkBytes.data(), 7 * 0xf8 + 0xd0, std::uint8_t(1));
        put(chunkBytes.data(), 7 * 0xf8 + 0xd4, std::uint32_t(128));
        put(chunkBytes.data(), 7 * 0xf8 + 0xe8, std::uint32_t(60));
        put(chunkBytes.data(), 7 * 0xf8 + 0xec, std::uint16_t(24));
        geometry.chunk = 7;
        geometry.indexCount = 60;
        context.rigid = reinterpret_cast<std::uint64_t>(rigidBytes.data());
        context.skinned = reinterpret_cast<std::uint64_t>(skinnedBytes.data());
        context.rigidCapacity = context.skinCapacity = 8;
        context.geometry = reinterpret_cast<std::uint64_t>(&geometry);
        GlassFg::originalRun = &fixtureRun;
        GlassFg::originalAppend = &fixtureAppend;
        GlassFg::originalRigid = &fixtureRigid;
        GlassFg::originalSkinned = &fixtureSkinned;
        GlassFg::originalUpload = &fixtureUpload;
        GlassFg::activeDrawState.store(fixture.get());
        GlassFg::run(nullptr, nullptr, nullptr);
        require(!GlassFg::currentBatch && fixture->occupied == 0 && !GlassFg::currentFlush, "Scope cleanup");
        require(forwardedAppends == 19 && forwardedFlushes == 12, "Original operations were dropped or repeated");
        GlassFg::GeometryDrawBatch batch;
        for (unsigned i = 0; i < 2049; ++i)
            batch.append(i, i + 1, { {}, 0, 1, i, false });
        require(batch.view(2049).empty(), "Bounded span overflow admitted");
        std::array<std::unique_ptr<GlassFg::RunScope>, 17> scopes;
        for (auto& scope : scopes)
            scope = std::make_unique<GlassFg::RunScope>(fixture.get());
        require(!GlassFg::currentBatch, "Pool exhaustion inherited prior batch");
        for (auto i = scopes.size(); i > 0; --i)
            scopes[i - 1].reset();
        require(!GlassFg::currentBatch && fixture->occupied == 0, "Nested scope restoration");
        GlassFg::activeDrawState.store(nullptr);
        printf("PASS direct_packet_slots=1 coincident_objects=1 batch_reorder=1 rigid_and_skinned=1 "
               "unknown_intervals_preserved=1 global_range=1 failed_upload_rejected=1 lifetime_reuse_rejected=1 "
               "mixed_frame_rejected=1 borrowed_view_scope=1 bounded_pool=1 original_calls_preserved=1 "
               "single_visible_cluster_rejected=1 game_hooks_installed=0\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        GlassFg::activeDrawState.store(nullptr);
        fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
