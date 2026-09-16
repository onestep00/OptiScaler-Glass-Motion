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
    explicit NgxParameterProbe(unsigned path) noexcept : instancePath(path) {}
    // The provider walks far more than the six inputs this correction knows
    // about: one Ray Reconstruction evaluation alone reads over a hundred
    // names. The previous 64-slot table saturated and silently dropped every
    // later key, which is exactly the set the DLSS-G keys belong to, so the
    // trace could not answer whether the generator reads DLSSG.MVecs.
    static constexpr unsigned kMaxKeys = 192;
    // Bounded per-call print: the interesting names first, then a sample of the
    // rest. A hundred-line log entry per evaluation is not usable.
    static constexpr unsigned kMaxCallKeys = 28;

    void bind(NVSDK_NGX_Parameter* real, FILE* sink, unsigned index, unsigned count, unsigned long long frame,
              ID3D12Resource* composedMotion, ID3D12Resource* composedDepth, ID3D12Resource* engineMotion,
              ID3D12Resource* engineDepth, unsigned path) noexcept
    {
        target = real;
        sinkLog = sink;
        serial = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        reads = 0;
        writes = 0;
        callIndex = index;
        callCount = count;
        callFrame = frame;
        callPath = path;
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
        std::fprintf(sink, "GLASS_PARAM_CALL path=%s serial=%llu index=%u count=%u frame=%llu reads=%u writes=%u",
                     callPath == 0 ? "dlssg" : "alias",
                     static_cast<unsigned long long>(serial), callIndex, callCount,
                     static_cast<unsigned long long>(callFrame), reads.load(std::memory_order_relaxed),
                     writes.load(std::memory_order_relaxed));
        unsigned printed = 0;
        // Pass 1: the inputs this module substitutes and the DLSS-G names the
        // correction is about. Pass 2 fills the remainder of the budget.
        for (unsigned pass = 0; pass < 2 && printed < kMaxCallKeys; ++pass)
        {
            for (unsigned i = 0; i < kMaxKeys && printed < kMaxCallKeys; ++i)
            {
                if (keys[i].lastCall.load(std::memory_order_relaxed) != serial)
                    continue;
                const bool important = importantKey(keys[i].text);
                if ((pass == 0) != important)
                    continue;
                dump(sink, keys[i]);
                ++printed;
            }
        }
        std::fprintf(sink, "\n");
    }

    // Full table, printed on a slow cadence.
    void report(FILE* sink) const noexcept
    {
        if (!sink)
            return;
        std::fprintf(sink, "GLASS_PARAM_TABLE path=%s calls=%llu distinct=%u overflow=%llu",
                     instancePath == 0 ? "dlssg" : "alias",
                     static_cast<unsigned long long>(calls.load(std::memory_order_relaxed)),
                     distinct.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(lost.load(std::memory_order_relaxed)));
        if (lostSamples.load(std::memory_order_relaxed) != 0)
        {
            std::fprintf(sink, " lost_sample=");
            for (unsigned i = 0; i < kLostSamples; ++i)
                if (lostText[i][0] != '\0')
                    std::fprintf(sink, "%s%s", i == 0 ? "" : ",", lostText[i]);
        }
        std::fprintf(sink, "\n");
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
        // Hash-indexed lookup. The provider reads every key it knows on each
        // evaluation, so a linear scan over 192 entries with a strcmp each was
        // the dominant cost of the trace itself.
        if (name == nullptr)
            return nullptr;
        const auto start = static_cast<unsigned>(hash(name) % kMaxKeys);
        KeyEntry* empty = nullptr;
        for (unsigned step = 0; step < kMaxKeys; ++step)
        {
            const auto i = (start + step) % kMaxKeys;
            const auto* known = keys[i].name.load(std::memory_order_relaxed);
            if (known == name)
                return &keys[i];
            if (known != nullptr && std::strcmp(known, name) == 0)
                return &keys[i];
            if (known == nullptr)
            {
                if (!empty)
                    empty = &keys[i];
                // Stop at the first empty slot on the probe sequence: a later
                // slot on the same sequence can only be occupied by an entry
                // inserted after this hole existed, and the insertion path
                // always uses the same rule.
                break;
            }
        }
        if (!empty)
            return nullptr;
        const char* expected = nullptr;
        if (!keys[empty - keys].name.compare_exchange_strong(expected, name, std::memory_order_relaxed))
            return slot(name);
        // Write the text before publishing anything else that depends on it.
        std::snprintf(empty->text, sizeof(empty->text), "%s", name);
        distinct.fetch_add(1, std::memory_order_relaxed);
        return empty;
    }

    // Bounded sample of names that did not fit, so a saturated table says what
    // it dropped instead of only how many notes were lost.
    void noteLost(const char* name) const noexcept
    {
        const auto index = lostSamples.fetch_add(1, std::memory_order_relaxed);
        if (index < kLostSamples && name != nullptr)
            std::snprintf(lostText[index], sizeof(lostText[0]), "%s", name);
    }

    static unsigned long long hash(const char* name) noexcept
    {
        unsigned long long value = 1469598103934665603ull;
        for (const char* cursor = name; *cursor != '\0'; ++cursor)
        {
            value ^= static_cast<unsigned char>(*cursor);
            value *= 1099511628211ull;
        }
        return value;
    }

    const char* matchOf(unsigned long long pointer) const noexcept
    {
        if (pointer == 0)
            return "null";
        const auto* resource = reinterpret_cast<ID3D12Resource*>(pointer);
        if (resource == watchMotion || resource == watchDepth || resource == watchLayerMvecs ||
            resource == watchLayerOpacity)
            return "composed";
        if (resource == watchEngineMotion || resource == watchEngineDepth)
            return "engine";
        return "other";
    }

    void dump(FILE* sink, const KeyEntry& entry) const noexcept
    {
        const auto type = entry.lastType.load(std::memory_order_relaxed);
        std::fprintf(sink, " [%s", entry.text);
        if (type == 6 || type == 7)
        {
            const auto pointer = entry.lastPointer.load(std::memory_order_relaxed);
            std::fprintf(sink, " ptr=%p match=%s", reinterpret_cast<void*>(pointer), matchOf(pointer));
        }
        else if (type < 6)
            std::fprintf(sink, " value=%llu", entry.lastValue.load(std::memory_order_relaxed));
        std::fprintf(sink, "]");
    }

    static bool importantKey(const char* name) noexcept
    {
        if (name == nullptr)
            return false;
        struct Marker
        {
            const char* text;
        };
        static constexpr Marker markers[] { { "MVec" },    { "MotionVectors" }, { "Depth" },
                                            { "HUDLess" }, { "Transparency" },  { "Backbuffer" } };
        for (const auto& marker : markers)
            if (std::strstr(name, marker.text) != nullptr)
                return true;
        return false;
    }

    void note(const char* name, int type, NVSDK_NGX_Result result, unsigned long long pointer) const noexcept
    {
        auto* entry = slot(name);
        if (!entry)
        {
            lost.fetch_add(1, std::memory_order_relaxed);
            noteLost(name);
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
    mutable std::atomic<unsigned> reads { 0 }, writes { 0 }, distinct { 0 }, lostSamples { 0 };
    static constexpr unsigned kLostSamples = 8;
    mutable char lostText[kLostSamples][48] {};
    mutable unsigned long long serial = 0;
    mutable unsigned callIndex = 0, callCount = 0;
    mutable unsigned long long callFrame = 0;
    mutable unsigned callPath = 0;
    unsigned instancePath = 0;
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
// past the call, so the object must not be a stack temporary. Path 0 is the
// engine-tagged DLSS-G evaluation, path 1 the alias (MotionVectors/Depth)
// evaluations the other NGX features go through. They are counted apart
// because the shared key table cannot say which feature read a name.
inline NgxParameterProbe& NgxParameterProbeInstance(unsigned path) noexcept
{
    static NgxParameterProbe probes[2] { NgxParameterProbe(0), NgxParameterProbe(1) };
    return probes[path == 0 ? 0 : 1];
}
} // namespace GlassFg
