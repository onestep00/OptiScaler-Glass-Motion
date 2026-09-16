#pragma once
// Observes the parameter table the real frame-generation provider reads during
// one NGX evaluation.
//
// The correction substitutes the motion and depth resources in the parameter
// table and then calls the provider. Whether the provider reads those keys at
// all cannot be told from the outside: a substituted resource that is never
// read looks exactly like a correction that has no effect. This probe sits in
// that call, forwards every access to the real table, and records the key name,
// the accessor type, and - for resource reads - whether the returned pointer is
// the composed texture, the engine's own texture, or something else.
//
// Only installed while the live trace switch is on, so the normal path keeps
// calling the provider with the original pointer.
#include <atomic>
#include <cstdio>
#include <cstring>
#include "nvsdk_ngx_params.h"

namespace GlassFg
{
class NgxParameterProbe final : public NVSDK_NGX_Parameter
{
  public:
    static constexpr unsigned kMaxKeys = 64;

    void bind(NVSDK_NGX_Parameter* real, FILE* sink, unsigned index, unsigned count, unsigned long long frame,
              ID3D12Resource* composedMotion, ID3D12Resource* composedDepth, ID3D12Resource* engineMotion,
              ID3D12Resource* engineDepth) noexcept
    {
        target = real;
        sinkLog = sink;
        serial = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        reads = 0;
        writes = 0;
        callIndex = index;
        callCount = count;
        callFrame = frame;
        watchMotion = composedMotion;
        watchDepth = composedDepth;
        watchEngineMotion = engineMotion;
        watchEngineDepth = engineDepth;
    }

    // Answer the motion and depth keys with the composed textures instead of the
    // table's own pointers. The provider copies input pointers out of the table
    // on some evaluations and reuses that copy on the others, so a Set/restore
    // around the call reaches only the evaluations that copy.
    void supply(ID3D12Resource* motion, ID3D12Resource* depth, const char* motionKey, const char* depthKey,
                ID3D12Resource* layerMvecs = nullptr, ID3D12Resource* layerOpacity = nullptr) noexcept
    {
        supplyMotion = motion;
        supplyDepth = depth;
        supplyMotionKey = motionKey;
        supplyDepthKey = depthKey;
        supplyLayerMvecs = layerMvecs;
        supplyLayerOpacity = layerOpacity;
        watchLayerMvecs = layerMvecs;
        watchLayerOpacity = layerOpacity;
    }

    // Keys the provider touched during the call that just returned, plus the
    // resource match for each. Bounded: one line per evaluation only while the
    // caller still wants them.
    void finish(FILE* sink) const noexcept
    {
        if (!sink)
            return;
        std::fprintf(sink, "GLASS_PARAM_CALL serial=%llu index=%u count=%u frame=%llu reads=%u writes=%u",
                     static_cast<unsigned long long>(serial), callIndex, callCount,
                     static_cast<unsigned long long>(callFrame), reads.load(std::memory_order_relaxed),
                     writes.load(std::memory_order_relaxed));
        for (unsigned i = 0; i < kMaxKeys; ++i)
        {
            if (keys[i].lastCall.load(std::memory_order_relaxed) != serial)
                continue;
            std::fprintf(sink, " [%s", keys[i].text);
            if (keys[i].lastType.load(std::memory_order_relaxed) == 6 || keys[i].lastType.load(std::memory_order_relaxed) == 7)
            {
                const auto pointer = keys[i].lastPointer.load(std::memory_order_relaxed);
                const char* match = "other";
                if (pointer != 0 && (reinterpret_cast<ID3D12Resource*>(pointer) == watchMotion ||
                                     reinterpret_cast<ID3D12Resource*>(pointer) == watchDepth ||
                                     reinterpret_cast<ID3D12Resource*>(pointer) == watchLayerMvecs ||
                                     reinterpret_cast<ID3D12Resource*>(pointer) == watchLayerOpacity))
                    match = "composed";
                else if (pointer != 0 && (reinterpret_cast<ID3D12Resource*>(pointer) == watchEngineMotion ||
                                          reinterpret_cast<ID3D12Resource*>(pointer) == watchEngineDepth))
                    match = "engine";
                std::fprintf(sink, " ptr=%p match=%s", reinterpret_cast<void*>(pointer), match);
            }
            else if (keys[i].lastType.load(std::memory_order_relaxed) < 6)
                std::fprintf(sink, " value=%llu", keys[i].lastValue.load(std::memory_order_relaxed));
            std::fprintf(sink, "]");
        }
        std::fprintf(sink, "\n");
    }

    // Full table, printed on a slow cadence.
    void report(FILE* sink) const noexcept
    {
        if (!sink)
            return;
        std::fprintf(sink, "GLASS_PARAM_TABLE calls=%llu distinct=%u overflow=%llu\n",
                     static_cast<unsigned long long>(calls.load(std::memory_order_relaxed)),
                     distinct.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(lost.load(std::memory_order_relaxed)));
        for (unsigned i = 0; i < kMaxKeys; ++i)
        {
            if (keys[i].name.load(std::memory_order_relaxed) == nullptr)
                continue;
            std::fprintf(sink, "  key=%-28s gets=%u sets=%u composed=%u engine=%u last=%p\n", keys[i].text,
                         keys[i].gets.load(std::memory_order_relaxed), keys[i].sets.load(std::memory_order_relaxed),
                         keys[i].composedHits.load(std::memory_order_relaxed),
                         keys[i].engineHits.load(std::memory_order_relaxed),
                         reinterpret_cast<void*>(keys[i].lastPointer.load(std::memory_order_relaxed)));
        }
        std::fflush(sink);
    }

    void Set(const char* name, unsigned long long value) override
    {
        ++writes;
        if (target)
            target->Set(name, value);
    }
    void Set(const char* name, float value) override
    {
        ++writes;
        if (target)
            target->Set(name, value);
    }
    void Set(const char* name, double value) override
    {
        ++writes;
        if (target)
            target->Set(name, value);
    }
    void Set(const char* name, unsigned int value) override
    {
        ++writes;
        if (target)
            target->Set(name, value);
    }
    void Set(const char* name, int value) override
    {
        ++writes;
        if (target)
            target->Set(name, value);
    }
    void Set(const char* name, ID3D11Resource* value) override
    {
        ++writes;
        if (target)
            target->Set(name, value);
    }
    void Set(const char* name, ID3D12Resource* value) override
    {
        ++writes;
        note(name, 8, NVSDK_NGX_Result_Success, reinterpret_cast<unsigned long long>(value));
        if (target)
            target->Set(name, value);
    }
    void Set(const char* name, void* value) override
    {
        ++writes;
        if (target)
            target->Set(name, value);
    }

    NVSDK_NGX_Result Get(const char* name, unsigned long long* value) const override
    {
        const auto result = target ? target->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter;
        note(name, 0, result, result == NVSDK_NGX_Result_Success && value ? *value : 0ull);
        return result;
    }
    NVSDK_NGX_Result Get(const char* name, float* value) const override
    {
        const auto result = target ? target->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter;
        note(name, 1, result, result == NVSDK_NGX_Result_Success && value ? bits(*value) : 0ull);
        return result;
    }
    NVSDK_NGX_Result Get(const char* name, double* value) const override
    {
        const auto result = target ? target->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter;
        unsigned long long observed = 0;
        if (result == NVSDK_NGX_Result_Success && value)
            std::memcpy(&observed, value, sizeof(observed));
        note(name, 2, result, observed);
        return result;
    }
    NVSDK_NGX_Result Get(const char* name, unsigned int* value) const override
    {
        const auto result = target ? target->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter;
        note(name, 3, result, result == NVSDK_NGX_Result_Success && value ? *value : 0ull);
        return result;
    }
    NVSDK_NGX_Result Get(const char* name, int* value) const override
    {
        const auto result = target ? target->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter;
        note(name, 4, result, result == NVSDK_NGX_Result_Success && value ? static_cast<unsigned long long>(*value) : 0ull);
        return result;
    }
    NVSDK_NGX_Result Get(const char* name, ID3D11Resource** value) const override
    {
        const auto result = target ? target->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter;
        note(name, 5, result, result == NVSDK_NGX_Result_Success && value
                                   ? reinterpret_cast<unsigned long long>(*value)
                                   : 0ull);
        return result;
    }
    NVSDK_NGX_Result Get(const char* name, ID3D12Resource** value) const override
    {
        const auto result = target ? target->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter;
        if (value != nullptr && result == NVSDK_NGX_Result_Success)
        {
            if (auto* replacement = substitute(name))
                *value = replacement;
        }
        note(name, 6, result, result == NVSDK_NGX_Result_Success && value
                                   ? reinterpret_cast<unsigned long long>(*value)
                                   : 0ull);
        return result;
    }
    NVSDK_NGX_Result Get(const char* name, void** value) const override
    {
        const auto result = target ? target->Get(name, value) : NVSDK_NGX_Result_FAIL_InvalidParameter;
        if (value != nullptr && result == NVSDK_NGX_Result_Success)
        {
            if (auto* replacement = substitute(name))
                *value = replacement;
        }
        note(name, 7, result, result == NVSDK_NGX_Result_Success && value
                                   ? reinterpret_cast<unsigned long long>(*value)
                                   : 0ull);
        return result;
    }

    void Reset() override
    {
        if (target)
            target->Reset();
    }

  private:
    ID3D12Resource* substitute(const char* name) const noexcept
    {
        if (name == nullptr)
            return nullptr;
        // The transparency layer is the provider's own slot for surfaces that
        // are blended into the colour buffer. It is queried every evaluation and
        // the game leaves it empty, so answering it never fights the engine.
        if (supplyLayerMvecs != nullptr && std::strcmp(name, "DLSS.TransparencyLayerMvecs") == 0)
            return supplyLayerMvecs;
        if (supplyLayerOpacity != nullptr && std::strcmp(name, "DLSS.TransparencyLayerOpacity") == 0)
            return supplyLayerOpacity;
        if (supplyMotion != nullptr && keyMatches(name, supplyMotionKey, "MotionVectors", "DLSSG.MVecs"))
            return supplyMotion;
        if (supplyDepth != nullptr && keyMatches(name, supplyDepthKey, "Depth", "DLSSG.Depth"))
            return supplyDepth;
        return nullptr;
    }

    static bool keyMatches(const char* name, const char* primary, const char* first, const char* second) noexcept
    {
        return (primary != nullptr && std::strcmp(name, primary) == 0) || std::strcmp(name, first) == 0 ||
               std::strcmp(name, second) == 0;
    }

    struct KeyEntry
    {
        std::atomic<const char*> name { nullptr };
        char text[48] {};
        std::atomic<unsigned> gets { 0 }, sets { 0 }, composedHits { 0 }, engineHits { 0 };
        std::atomic<unsigned long long> lastPointer { 0 }, lastValue { 0 };
        std::atomic<unsigned long long> lastCall { 0 };
        std::atomic<int> lastType { -1 };
    };

    KeyEntry* slot(const char* name) const noexcept
    {
        KeyEntry* empty = nullptr;
        for (unsigned i = 0; i < kMaxKeys; ++i)
        {
            const auto* known = keys[i].name.load(std::memory_order_relaxed);
            if (known == name)
                return &keys[i];
            if (known == nullptr)
            {
                if (!empty)
                    empty = &keys[i];
                continue;
            }
            if (name != nullptr && std::strcmp(known, name) == 0)
                return &keys[i];
        }
        if (!empty || name == nullptr)
            return nullptr;
        const char* expected = nullptr;
        if (!keys[empty - keys].name.compare_exchange_strong(expected, name, std::memory_order_relaxed))
            return slot(name);
        std::snprintf(empty->text, sizeof(empty->text), "%s", name);
        distinct.fetch_add(1, std::memory_order_relaxed);
        return empty;
    }

    void note(const char* name, int type, NVSDK_NGX_Result result, unsigned long long pointer) const noexcept
    {
        auto* entry = slot(name);
        if (!entry)
        {
            lost.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (type < 8)
            entry->gets.fetch_add(1, std::memory_order_relaxed);
        else
            entry->sets.fetch_add(1, std::memory_order_relaxed);
        entry->lastCall.store(serial, std::memory_order_relaxed);
        entry->lastType.store(type, std::memory_order_relaxed);
        entry->lastValue.store(pointer, std::memory_order_relaxed);
        entry->lastPointer.store(pointer, std::memory_order_relaxed);
        if ((type == 6 || type == 7 || type == 8) && result == NVSDK_NGX_Result_Success && pointer != 0)
        {
            const auto* resource = reinterpret_cast<ID3D12Resource*>(pointer);
            if (resource == watchMotion || resource == watchDepth)
                entry->composedHits.fetch_add(1, std::memory_order_relaxed);
            else if (resource == watchEngineMotion || resource == watchEngineDepth)
                entry->engineHits.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static unsigned long long bits(float value) noexcept
    {
        unsigned int raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        return raw;
    }

    mutable KeyEntry keys[kMaxKeys] {};
    mutable std::atomic<unsigned long long> calls { 0 }, lost { 0 };
    mutable std::atomic<unsigned> reads { 0 }, writes { 0 }, distinct { 0 };
    mutable unsigned long long serial = 0;
    mutable unsigned callIndex = 0, callCount = 0;
    mutable unsigned long long callFrame = 0;
    mutable NVSDK_NGX_Parameter* target = nullptr;
    mutable FILE* sinkLog = nullptr;
    mutable ID3D12Resource* supplyMotion = nullptr;
    mutable ID3D12Resource* supplyDepth = nullptr;
    mutable ID3D12Resource* supplyLayerMvecs = nullptr;
    mutable ID3D12Resource* supplyLayerOpacity = nullptr;
    mutable const char* supplyMotionKey = nullptr;
    mutable const char* supplyDepthKey = nullptr;
    mutable ID3D12Resource* watchMotion = nullptr;
    mutable ID3D12Resource* watchDepth = nullptr;
    mutable ID3D12Resource* watchLayerMvecs = nullptr;
    mutable ID3D12Resource* watchLayerOpacity = nullptr;
    mutable ID3D12Resource* watchEngineMotion = nullptr;
    mutable ID3D12Resource* watchEngineDepth = nullptr;
};

// One instance for the life of the module: the provider may keep the pointer
// past the call, so the object must not be a stack temporary.
inline NgxParameterProbe& NgxParameterProbeInstance() noexcept
{
    static NgxParameterProbe probe;
    return probe;
}
} // namespace GlassFg
