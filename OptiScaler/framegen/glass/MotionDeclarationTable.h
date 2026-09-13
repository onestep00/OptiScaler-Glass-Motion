#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>

namespace GlassFg {

// Engine metadata only. This table stores no object pointer, transform history,
// GPU allocation or frame data. Configure once before enabling the provider hook.
class MotionDeclarationTable {
public:
    static constexpr uint32_t MaxDeclarations = 256;
    static constexpr uint32_t MaxNames = 4096;
    static constexpr uint32_t MaxNamesPerDeclaration = 32;
    static constexpr uint64_t MotionName = 0x5fc6bea24112bd53ULL;

    struct Name {
        uint64_t hash = 0;
        uint8_t index = 0;
        std::array<uint8_t, 7> reserved{};
    };
    struct Plan {
        uint64_t key = 0;
        uint32_t mask = 0;
        uint16_t nameOffset = 0;
        uint8_t nameCount = 0;
        uint8_t motionRow = 0;
    };
    struct Metadata {
        uint64_t key = 0;
        uint32_t mask = 0;
        uint32_t opaque = 0;
        uint64_t names = 0;
        uint32_t capacity = 0;
        uint32_t count = 0;
    };
    static_assert(sizeof(Name) == 16 && sizeof(Plan) == 16 && sizeof(Metadata) == 32);

    enum class State : uint8_t {
        NotSelected, Unreadable, Mismatch, Prepared, Reused, Revalidated
    };
    struct Result {
        const Metadata* metadata = nullptr;
        State state = State::NotSelected;
    };

    bool configure(std::span<const Plan> plans, std::span<const Name> names) {
        std::lock_guard lock(initialization_);
        if (count_.load(std::memory_order_relaxed) || plans.empty() ||
            plans.size() > MaxDeclarations || names.size() > MaxNames)
            return false;
        uint32_t offset = 0;
        for (size_t i = 0; i < plans.size(); ++i) {
            const auto& plan = plans[i];
            if ((i && plans[i - 1].key >= plan.key) || plan.nameOffset != offset ||
                plan.nameCount >= MaxNamesPerDeclaration || plan.motionRow > 24 ||
                offset + plan.nameCount > names.size())
                return false;
            for (uint32_t n = 0; n < plan.nameCount; ++n) {
                const auto& name = names[offset + n];
                if (name.hash == MotionName) return false; // Never duplicate a native motion declaration.
                for (uint32_t j = 0; j < n; ++j)
                    if (names[offset + j].hash == name.hash) return false;
            }
            offset += plan.nameCount;
        }
        if (offset != names.size()) return false;
        for (size_t i = 0; i < plans.size(); ++i) {
            auto& entry = entries_[i];
            entry.plan = plans[i];
            entry.cloneOffset = static_cast<uint32_t>(plans[i].nameOffset + i);
            for (uint32_t n = 0; n < plans[i].nameCount; ++n) {
                auto name = names[plans[i].nameOffset + n];
                name.reserved = {};
                expectedNames_[plans[i].nameOffset + n] = name;
                clonedNames_[entry.cloneOffset + n] = name;
            }
            clonedNames_[entry.cloneOffset + plans[i].nameCount] = Name{MotionName, plans[i].motionRow, {}};
        }
        count_.store(static_cast<uint32_t>(plans.size()), std::memory_order_release);
        return true;
    }

    // The adapter supplies a bounded, guarded read. The source metadata must be
    // borrowed from the sourceRecord provider callback, not a retained game pointer.
    template<class Read>
    Result rewrite(uint64_t key, const void* source, Read&& read) {
        uint32_t lo = 0, hi = count_.load(std::memory_order_acquire);
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (entries_[mid].plan.key < key) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= count_.load(std::memory_order_relaxed) || entries_[lo].plan.key != key)
            return {};
        auto& entry = entries_[lo];
        Metadata sourceRecord{};
        if (!read(reinterpret_cast<uint64_t>(source), &sourceRecord, sizeof(sourceRecord)))
            return {nullptr, State::Unreadable};
        const auto& plan = entry.plan;
        if (sourceRecord.key != key || sourceRecord.mask != plan.mask || sourceRecord.count != plan.nameCount ||
            sourceRecord.capacity < sourceRecord.count || (sourceRecord.count && !sourceRecord.names))
            return {nullptr, State::Mismatch};
        std::array<Name, MaxNamesPerDeclaration> observed{};
        if (sourceRecord.count && !read(sourceRecord.names, observed.data(), sourceRecord.count * sizeof(Name)))
            return {nullptr, State::Unreadable};
        // Reordering/relocation may change across loads. Compare declared names
        // and indices rather than treating the old storage pointer as identity.
        for (uint32_t n = 0; n < sourceRecord.count; ++n) {
            const auto& expectedName = expectedNames_[plan.nameOffset + n];
            uint32_t matches = 0;
            for (uint32_t j = 0; j < sourceRecord.count; ++j)
                matches += observed[j].hash == expectedName.hash && observed[j].index == expectedName.index;
            if (matches != 1) return {nullptr, State::Mismatch};
        }
        Metadata after{};
        if (!read(reinterpret_cast<uint64_t>(source), &after, sizeof(after)))
            return {nullptr, State::Unreadable};
        if (std::memcmp(&sourceRecord, &after, sizeof(after))) return {nullptr, State::Mismatch};
        if (entry.prepared.load(std::memory_order_acquire)) {
            if (sourceRecord.opaque != entry.sourceRecord.opaque) return {nullptr, State::Mismatch};
            return {&entry.augmented, !std::memcmp(&sourceRecord, &entry.sourceRecord, sizeof(sourceRecord))
                                        ? State::Reused : State::Revalidated};
        }
        std::lock_guard lock(initialization_);
        if (entry.prepared.load(std::memory_order_relaxed)) {
            if (sourceRecord.opaque != entry.sourceRecord.opaque) return {nullptr, State::Mismatch};
            return {&entry.augmented, State::Revalidated};
        }
        entry.sourceRecord = sourceRecord;
        entry.augmented = sourceRecord;
        entry.augmented.mask |= 1u << 7;
        entry.augmented.names = reinterpret_cast<uint64_t>(&clonedNames_[entry.cloneOffset]);
        entry.augmented.count = sourceRecord.count + 1;
        entry.augmented.capacity = entry.augmented.count;
        entry.prepared.store(true, std::memory_order_release);
        return {&entry.augmented, State::Prepared};
    }

    uint32_t count() const noexcept { return count_.load(std::memory_order_acquire); }

private:
    struct Entry {
        Plan plan{};
        Metadata sourceRecord{}, augmented{};
        uint32_t cloneOffset = 0;
        std::atomic<bool> prepared{false};
    };
    std::array<Entry, MaxDeclarations> entries_{};
    std::array<Name, MaxNames> expectedNames_{};
    std::array<Name, MaxNames + MaxDeclarations> clonedNames_{};
    std::atomic<uint32_t> count_{0};
    std::mutex initialization_;
};

}
