// Standalone CPU benchmark for GeometryPipelineCache::find, the largest single
// stage of the draw hook (measured in game as indexed_pipeline_ns).
//
// The game is not needed: the benchmark builds a real cache on a real D3D12
// device, registers real pipeline states with the same creation path the
// render hook uses, and then replays lookup patterns whose key counts and
// call counts come from the live counters (973 compiled pipelines,
// ~6,285 DrawIndexedInstanced calls per engine frame). Patterns cover the
// memo-friendly (same key), memo-hostile (round robin) and realistic (random
// and hot/cold) shapes, single threaded and contended.
//
// Usage: PipelineLookupBench <fixtureDir> <dxcompiler.dll> [--keys N]
//        [--calls M] [--threads T] [--frame-calls N] [--burst N]
#include "GeometryTestDevice.h"
#include "../GeometryPipelineCache.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
// Live counters: 6,285 DrawIndexedInstanced calls per engine frame (4x MFG
// scene). Overridable with --frame-calls so a heavier scene can be projected.
constexpr unsigned DefaultIndexedCallsPerFrame = 6285;
// Zipf exponent for the skewed pattern. A scene compiles ~1,000-4,000
// pipelines, but a handful of them cover most draws, so a uniform random
// pattern understates the memo hit rate.
constexpr double ZipfExponent = 1.1;
// Runs of identical instance keys: a grouped draw repeats its pipeline per
// element, so the same key can arrive several times in a row.
constexpr unsigned DefaultBurstLength = 8;

enum PatternId : unsigned
{
    PatternSameKey = 0,
    PatternRoundRobin = 1,
    PatternRandom = 2,
    PatternHotCold = 3,
    PatternZipf = 4,
    PatternBurst = 5,
    PatternMixed = 6,
    // Control: the skewed sampler without the cache lookup, so the zipf row can
    // be reported net of the key-selection cost instead of charging it to find.
    PatternZipfSampleOnly = 7
};

void step(const char* name)
{
    std::printf("STEP %s\n", name);
    std::fflush(stdout);
}

// Debug-layer messages are the only way to see why a PSO desc was rejected;
// the device reports E_INVALIDARG for every kind of mismatch.
void dumpInfoQueue(ID3D12Device* device)
{
    if (!device)
        return;
    ComPtr<ID3D12InfoQueue> queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
        return;
    const auto count = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T size = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &size)))
            continue;
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (SUCCEEDED(queue->GetMessage(i, message, &size)))
        {
            if (message->pDescription && message->DescriptionByteLength > 0)
                std::printf("D3D12_MSG %.*s\n", int(message->DescriptionByteLength), message->pDescription);
            else
                std::printf("D3D12_MSG id=%d (no description)\n", int(message->ID));
            std::fflush(stdout);
        }
    }
}
ID3D12Device* debugDevice = nullptr;

struct PatternResult
{
    double nsPerCall = 0.0;
    double resolvedPct = 0.0;
};

struct Result
{
    const char* name = "";
    double nsPerCall = 0.0;
    unsigned long long calls = 0;
    bool hits = false;
    double resolvedPct = 0.0;
};

PatternResult runPattern(const GlassFg::GeometryPipelineCache& cache, const char* name,
                         const std::vector<ID3D12PipelineState*>& keys, unsigned long long calls, unsigned threadCount,
                         unsigned pattern, bool* sawHit, unsigned hotKeys, const std::vector<double>& zipfCdf,
                         unsigned burstLength, unsigned long long frameCalls)
{
    std::atomic<unsigned long long> sink { 0 };
    std::atomic<unsigned long long> resolved { 0 };
    std::atomic<bool> start { false };
    const auto worker = [&](unsigned id)
    {
        std::mt19937 rng(0x9e3779b9u + id * 7919u);
        // Skewed sampler: one draw from the cumulative weights per lookup. The
        // 20-bit grid keeps the sequence deterministic and avoids a floating
        // point distribution call on the hot path.
        const auto zipfPick = [&]()
        {
            const double target = double(rng() & 0xFFFFFu) / double(0x100000u) * zipfCdf.back();
            const auto it = std::lower_bound(zipfCdf.begin(), zipfCdf.end(), target);
            return size_t(it == zipfCdf.end() ? zipfCdf.size() - 1 : size_t(it - zipfCdf.begin()));
        };
        const auto hotColdPick = [&]()
        {
            return (rng() % 10u) ? size_t(rng() % hotKeys)
                                 : size_t(hotKeys + rng() % (keys.size() - hotKeys));
        };
        const auto begin = Clock::now();
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        unsigned long long hits = 0, seen = 0;
        for (unsigned long long i = id; i < calls; i += threadCount)
        {
            size_t index = 0;
            switch (pattern)
            {
            case PatternSameKey: index = 0; break;
            case PatternRoundRobin: index = size_t(i % keys.size()); break;
            case PatternRandom: index = size_t(rng() % keys.size()); break;
            case PatternZipf: index = zipfPick(); break;
            case PatternZipfSampleOnly:
                // Control run: pay for the sampler, skip the lookup, so the
                // zipf row can be corrected by this cost.
                ++seen;
                sink.fetch_add(zipfPick(), std::memory_order_relaxed);
                continue;
            case PatternBurst: index = size_t((i / burstLength) % keys.size()); break;
            case PatternMixed:
                // One renderer thread per shape: skewed, ordered, hot/cold and
                // bursty. This is the contended case that is closest to the
                // renderer, where several threads walk different key sets.
                switch (id % 4u)
                {
                case 0: index = zipfPick(); break;
                case 1: index = size_t((i + size_t(id) * 97u) % keys.size()); break;
                case 2: index = hotColdPick(); break;
                default: index = size_t((i / burstLength) % keys.size()); break;
                }
                break;
            default: index = hotColdPick(); break;
            }
            // The hook reaches this through FindGeometryPipeline, which only
            // adds the published-control atomic loads; the cost under test is
            // the cache lookup itself.
            const auto lease = cache.find(keys[index]);
            hits += lease ? 1 : 0;
            ++seen;
            sink.fetch_add(lease ? 1 : 0, std::memory_order_relaxed);
        }
        const auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
        resolved.fetch_add(hits, std::memory_order_relaxed);
        if (id == 0 && sawHit)
            *sawHit = hits != 0;
        if (elapsed > 0.0)
            std::printf("  thread %u: %.1f ns/call over %llu calls (%.1f%% resolved)\n", id,
                        elapsed * 1.0e9 / double(seen), seen, double(hits) * 100.0 / double(seen));
    };
    std::vector<std::thread> threads;
    for (unsigned id = 0; id < threadCount; ++id)
        threads.emplace_back(worker, id);
    const auto begin = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
        thread.join();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    const auto perCall = elapsed * 1.0e9 / double(calls);
    const double pct = calls ? 100.0 * double(resolved.load()) / double(calls) : 0.0;
    std::printf("%-14s threads=%u calls=%llu ns/call=%.1f resolved=%.1f%% projected_ms_per_engine_frame=%.4f\n",
                name, threadCount, calls, perCall, pct, perCall * double(frameCalls) / 1.0e6);
    (void) sink.load();
    return { perCall, pct };
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc >= 3, "usage: PipelineLookupBench <fixtureDir> <dxcompiler.dll> [--keys N] [--calls M] "
                           "[--threads T] [--frame-calls N] [--burst N]");
        const std::filesystem::path fixtureDirectory = argv[1];
        unsigned keys = 64, threadCount = 4;
        unsigned long long calls = 400000;
        unsigned long long frameCalls = DefaultIndexedCallsPerFrame;
        unsigned burstLength = DefaultBurstLength;
        for (int i = 3; i + 1 < argc; i += 2)
        {
            const std::wstring flag = argv[i];
            const auto value = std::stoul(argv[i + 1]);
            if (flag == L"--keys")
                keys = unsigned(value);
            else if (flag == L"--calls")
                calls = value;
            else if (flag == L"--threads")
                threadCount = unsigned(value);
            else if (flag == L"--frame-calls")
                frameCalls = value;
            else if (flag == L"--burst")
                burstLength = unsigned(value);
            else
                require(false, "unknown flag");
        }
        require(keys >= 2, "at least two keys");
        require(threadCount >= 1 && threadCount <= 16, "thread count range");
        require(frameCalls >= 1, "frame call count range");
        require(burstLength >= 1, "burst length range");

        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                debug->EnableDebugLayer();
        }
        Device g;
        debugDevice = g.d.Get();
        step("device");
        // instances.dxil is the vertex shader whose input signature matches the
        // per-instance layout below; fixture.dxil carries a different one.
        const auto vs = read(fixtureDirectory / "instances.dxil");
        const auto ps = read(fixtureDirectory / "fixture-pixel.dxil");
        step("shaders");
        D3D12_ROOT_PARAMETER parameter {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = { 0, 0, 4 };
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rootDesc { 1, &parameter, 0, nullptr,
                                             D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                                 D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT };
        ComPtr<ID3DBlob> serialized, errors;
        check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        step("root-signature-blob");
        ComPtr<ID3D12RootSignature> root;
        check(g.d->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&root)));
        step("root-signature");
        D3D12_INPUT_ELEMENT_DESC layout[] {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "OBJECT_ID", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 }
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.Get();
        pd.VS = { vs.data(), vs.size() };
        pd.PS = { ps.data(), ps.size() };
        auto& blend = pd.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.InputLayout = { layout, 3 };
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;

        std::vector<ComPtr<ID3D12PipelineState>> states(keys);
        for (unsigned i = 0; i < keys; ++i)
        {
            const HRESULT hr = g.d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&states[i]));
            if (FAILED(hr))
            {
                std::printf("PSO_FAIL index=%u hr=0x%08lX\n", i, (unsigned long) hr);
                std::fflush(stdout);
                check(hr);
            }
            if (!states[i])
                require(false, "empty pipeline state");
        }
        step("pipeline-states");

        GlassFg::GeometryPipelineCache cache(g.d.Get(), std::filesystem::absolute(argv[2]),
                                            { keys + 8, keys + 8, 256 * 1024 * 1024 });
        step("cache");
        require(cache.rootCreated(root.Get(), 0, serialized->GetBufferPointer(), serialized->GetBufferSize()),
                "root registration");
        step("root-registered");
        // The compiler worker reads the shader bytes after pipelineCreated
        // returns, so the buffers stay alive until every job is finished.
        std::vector<std::vector<std::byte>> vertex(keys), pixel(keys);
        for (unsigned i = 0; i < keys; ++i)
        {
            vertex[i].resize(vs.size());
            pixel[i].resize(ps.size());
            memcpy(vertex[i].data(), vs.data(), vs.size());
            memcpy(pixel[i].data(), ps.data(), ps.size());
            auto supplied = pd;
            supplied.VS = { vertex[i].data(), vertex[i].size() };
            supplied.PS = { pixel[i].data(), pixel[i].size() };
            require(cache.pipelineCreated(states[i].Get(), supplied), "pipeline registration");
        }
        step("pipelines-registered");
        const auto deadline = GetTickCount64() + 240000;
        while (cache.stats().pending && GetTickCount64() < deadline)
            Sleep(1);
        const auto ready = cache.stats();
        std::printf("BENCH keys=%u compiled=%llu pending=%llu rejected=%llu bytes=%llu last_error=%s\n", keys,
                    ready.ready, ready.pending, ready.rejected, ready.retainedBytes,
                    ready.lastError.empty() ? "-" : ready.lastError.c_str());
        require(ready.pending == 0 && ready.ready == keys, "not every pipeline became ready");

        std::vector<ID3D12PipelineState*> pointers(keys);
        for (unsigned i = 0; i < keys; ++i)
            pointers[i] = states[i].Get();
        // Skewed weights: key 0 is the most reused pipeline state. Built once
        // and shared read-only by every thread.
        std::vector<double> zipfCdf(keys);
        double weightSum = 0.0;
        for (unsigned i = 0; i < keys; ++i)
        {
            weightSum += 1.0 / std::pow(double(i) + 1.0, ZipfExponent);
            zipfCdf[i] = weightSum;
        }
        const unsigned hotKeys =
            (std::max)(1u, (std::min)(unsigned(keys) / 8u, unsigned(keys) - 1u));
        bool sawHit = false;
        std::printf("BENCH_SHAPE keys=%u threads=%u calls=%llu frame_calls=%llu burst=%u zipf_s=%.2f hot_keys=%u\n",
                    keys, threadCount, calls, frameCalls, burstLength, ZipfExponent, hotKeys);
        runPattern(cache, "null", pointers, calls, 1, PatternSameKey, &sawHit, hotKeys, zipfCdf, burstLength,
                   frameCalls);

        std::vector<Result> results;
        const auto record = [&](const char* name, const PatternResult& pattern)
        {
            results.push_back({ name, pattern.nsPerCall, calls, true, pattern.resolvedPct });
        };
        record("same-key", runPattern(cache, "same-key", pointers, calls, 1, PatternSameKey, &sawHit, hotKeys,
                                      zipfCdf, burstLength, frameCalls));
        record("round-robin", runPattern(cache, "round-robin", pointers, calls, 1, PatternRoundRobin, &sawHit, hotKeys,
                                         zipfCdf, burstLength, frameCalls));
        record("random", runPattern(cache, "random", pointers, calls, 1, PatternRandom, &sawHit, hotKeys, zipfCdf,
                                    burstLength, frameCalls));
        record("hot/cold", runPattern(cache, "hot/cold", pointers, calls, 1, PatternHotCold, &sawHit, hotKeys, zipfCdf,
                                      burstLength, frameCalls));
        const auto zipf = runPattern(cache, "zipf", pointers, calls, 1, PatternZipf, &sawHit, hotKeys, zipfCdf,
                                     burstLength, frameCalls);
        record("zipf", zipf);
        const auto zipfSampler = runPattern(cache, "zipf-sample-only", pointers, calls, 1, PatternZipfSampleOnly,
                                            &sawHit, hotKeys, zipfCdf, burstLength, frameCalls);
        record("zipf-sampler", zipfSampler);
        record("burst", runPattern(cache, "burst", pointers, calls, 1, PatternBurst, &sawHit, hotKeys, zipfCdf,
                                   burstLength, frameCalls));
        record("random-mt", runPattern(cache, "random-mt", pointers, calls, threadCount, PatternRandom, &sawHit,
                                       hotKeys, zipfCdf, burstLength, frameCalls));
        record("mixed-mt", runPattern(cache, "mixed-mt", pointers, calls, threadCount, PatternMixed, &sawHit, hotKeys,
                                      zipfCdf, burstLength, frameCalls));
        for (const auto& result : results)
            std::printf("BENCH_RESULT %s ns_per_call=%.2f resolved_pct=%.1f ms_per_engine_frame_at_%u=%.4f\n",
                        result.name, result.nsPerCall, result.resolvedPct, unsigned(frameCalls),
                        result.nsPerCall * double(frameCalls) / 1.0e6);
        const double zipfNet = zipf.nsPerCall - zipfSampler.nsPerCall;
        std::printf("BENCH_RESULT zipf-net ns_per_call=%.2f note=zipf_minus_sampler ms_per_engine_frame_at_%u=%.4f\n",
                    zipfNet, unsigned(frameCalls), zipfNet * double(frameCalls) / 1.0e6);
        require(sawHit, "no lookup resolved");
        cache.stop();
        std::printf("PIPELINE_LOOKUP_BENCH_OK\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::printf("PIPELINE_LOOKUP_BENCH_FAILED %s\n", error.what());
        std::fflush(stdout);
        dumpInfoQueue(debugDevice);
        return 1;
    }
}
