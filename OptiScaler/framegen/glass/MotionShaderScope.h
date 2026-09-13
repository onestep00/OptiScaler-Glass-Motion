#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace GlassFg {
// Immutable cache-identity pairs. Consulted during native shader/layout creation,
// not per draw. A shared metadata key alone never authorizes a rewrite.
class MotionShaderScope {
public:
    static constexpr uint32_t MaxPairs = 4096;
    struct Pair { uint64_t vertex, partner, metadata; };
    static_assert(sizeof(Pair) == 24);
    bool configure(std::span<const Pair> input) noexcept {
        if (used || input.empty() || input.size() > pairs.size()) return false;
        for (size_t i = 0; i < input.size(); ++i) {
            const auto& p = input[i];
            if (!p.vertex || !p.partner || !p.metadata || p.vertex == p.partner ||
                (i && !less(input[i - 1], p))) return false;
        }
        std::copy(input.begin(), input.end(), pairs.begin());
        used = static_cast<uint32_t>(input.size());
        return true;
    }
    uint64_t lookup(uint64_t currentShader, uint64_t otherShader) const noexcept {
        const Pair key { currentShader, otherShader, 0 };
        const auto end = pairs.begin() + used;
        const auto at = std::lower_bound(pairs.begin(), end, key, less);
        return at != end && at->vertex == currentShader && at->partner == otherShader ? at->metadata : 0;
    }
    uint32_t count() const noexcept { return used; }
    // Nested/unsupported stage calls suppress the outer selection, then restore
    // it. Only the checked native metadata call site can consume the selection.
    struct Invocation {
        Invocation(uint64_t key, uint64_t caller) noexcept : previous(active), metadata(key), returnAddress(caller) {
            active = this;
        }
        ~Invocation() { active = previous; }
        Invocation(const Invocation&) = delete;
        Invocation& operator=(const Invocation&) = delete;
        static bool allows(uint64_t key, uint64_t caller) noexcept {
            return active && active->metadata && key == active->metadata && caller == active->returnAddress;
        }
    private:
        const Invocation* previous;
        uint64_t metadata, returnAddress;
        static inline thread_local const Invocation* active = nullptr;
    };
private:
    static bool less(const Pair& a, const Pair& b) noexcept {
        return a.vertex < b.vertex || (a.vertex == b.vertex && a.partner < b.partner);
    }
    std::array<Pair, MaxPairs> pairs {};
    uint32_t used = 0;
};
}
