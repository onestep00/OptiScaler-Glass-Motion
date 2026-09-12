#include "../VertexHistoryCache.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

static void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
static GlassFg::VertexHistoryKey key(unsigned id)
{
    return { { 0x1000ull + id, 0x2000, id, 1 }, 1, 2, 3, 0, 4 };
}
static bool overlaps(GlassFg::VertexHistoryAllocation a, GlassFg::VertexHistoryAllocation b)
{
    return a.base < b.base + b.capacity && b.base < a.base + a.capacity;
}
int main()
{
    using namespace GlassFg;
    // One forced-collision set makes reuse rules observable independently of
    // the hash distribution. Completed N-1 and recorded current work stay live.
    VertexHistoryCache<1, 4, 16, 4> cache;
    require(cache.beginFrame(1, 0, 0), "first frame rejected");
    auto original = key(1), chunk = original, factory = original, view = original;
    chunk.chunk = 1; factory.vertexFactory = 9; view.view = 8;
    const auto a = cache.acquire(original, 3, 1), b = cache.acquire(chunk, 5, 1);
    const auto c = cache.acquire(factory, 1, 1), d = cache.acquire(view, 4, 1);
    require(a && b && c && d, "separate history domains were rejected");
    require(!overlaps(a,b) && !overlaps(a,c) && !overlaps(a,d) && !overlaps(b,c) &&
            !overlaps(b,d) && !overlaps(c,d), "chunks/factories/views alias");
    require(!cache.acquire(key(20), 1, 1), "in-flight entry evicted");
    require(cache.beginFrame(2, 1, 4), "N-1 frame rejected");
    require(cache.liveEntries() == 4 && !cache.acquire(key(20), 1, 2), "completed N-1 evicted");
    require(cache.acquire(original, 3, 2).generation == a.generation, "consecutive history lost");
    require(cache.beginFrame(3, 1, 4), "third frame rejected");
    require(cache.liveEntries() == 1, "cold completed histories did not retire");
    auto replacement = cache.acquire(chunk, 5, 3);
    require(replacement && replacement.generation != b.generation && !overlaps(a, replacement),
            "reused storage admits old GPU generation");
    require(!cache.acquire(original, 4, 3), "changed topology admitted with old key");
    auto revised = original; revised.topology++;
    require(bool(cache.acquire(revised, 4, 3)), "new topology identity rejected");
    auto invalid = key(77); invalid.view = 0;
    require(!cache.acquire(invalid, 1, 3), "unknown view admitted");
    require(!cache.acquire(original, 3, 2) && !cache.beginFrame(2, 1), "stale frame admitted");
    require(!cache.beginFrame(4, 0) && !cache.beginFrame(4, 4), "invalid retirement admitted");

    // Exact arena exhaustion then coalescing, without borrowing live pages.
    VertexHistoryCache<1, 16, 16, 4> arena;
    require(arena.beginFrame(1, 0), "arena start rejected");
    for (unsigned i=0;i<16;++i) require(bool(arena.acquire(key(i), 1, 1)), "arena fill failed");
    require(arena.reservedVertices() == 64, "arena accounting mismatch");
    require(arena.beginFrame(4, 0, 16) && !arena.acquire(key(20), 64, 4), "unretired arena reused");
    require(arena.beginFrame(5, 1, 16), "retirement rejected");
    require(arena.liveEntries() == 0 && arena.reservedVertices() == 0, "arena did not reclaim");
    const auto whole = arena.acquire(key(50), 64, 5);
    require(whole && whole.base == 0 && whole.capacity == 64, "buddy coalescing failed");

    // Churn includes long GPU lag, cold scene replacements and multiple chunks
    // per object. Track every returned allocation while it may still be read.
    VertexHistoryCache<64, 4, 256, 16> churn;
    struct Live { VertexHistoryAllocation allocation; unsigned lastFrame; };
    std::vector<Live> live;
    std::mt19937 random(0x45678765);
    unsigned accepted = 0, rejected = 0;
    for (unsigned frame=1; frame<=300; ++frame)
    {
        const unsigned done = frame > 8 ? frame - 8 : 0;
        require(churn.beginFrame(frame, done, 7), "churn frame rejected");
        live.erase(std::remove_if(live.begin(),live.end(),[&](const auto& item) {
            return item.lastFrame < frame - 1 && item.lastFrame <= done;
        }),live.end());
        for(unsigned draw=0; draw<48; ++draw)
        {
            const unsigned id = random() % 192;
            auto object = key(id); object.chunk = id % 3;
            const auto allocation = churn.acquire(object, 1 + (id * 13) % 81, frame);
            if (!allocation) { ++rejected; continue; }
            ++accepted;
            bool found = false;
            for(auto& entry : live)
                if(entry.allocation.generation == allocation.generation)
                {
                    require(entry.allocation.base == allocation.base && entry.allocation.vertices == allocation.vertices,
                            "live generation relocated");
                    entry.lastFrame = frame; found = true;
                }
                else require(!overlaps(entry.allocation,allocation), "in-flight/history arena overlap");
            if(!found) live.push_back({allocation,frame});
        }
        require(churn.reservedVertices() <= 4096 && churn.liveEntries() <= 256, "cache exceeds fixed budget");
        require(churn.stats.lastSweepInspections <= 7 && churn.stats.maxLookupInspections <= 4,
                "per-frame/per-draw lookup budget exceeded");
    }
    require(accepted > 1000 && rejected > 0 && churn.stats.reclaimed > 100, "churn did not exercise pressure/reuse");
    // Construction is intentionally off the render path and may live on heap.
    const auto production = std::make_unique<VertexHistoryCache<>>();
    require(production->reservedVertices() == 0, "default cache not empty");
    std::cout << "PASS history identity/retirement/budget: 300 frames, " << accepted << " admissions, "
              << rejected << " bounded rejections; metadata_bytes=" << sizeof(*production) << '\n';
}
