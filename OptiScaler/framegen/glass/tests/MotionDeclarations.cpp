#include "../MotionDeclarationTable.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using Table = GlassFg::MotionDeclarationTable;
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static bool readOwned(uint64_t source, void* target, size_t bytes) {
    if (!source) return false;
    std::memcpy(target, reinterpret_cast<void*>(source), bytes);
    return true;
}
int main() {
    try {
        auto table = std::make_unique<Table>();
        std::array<Table::Plan, 256> plans{};
        std::array<Table::Name, 4096> names{};
        std::array<Table::Metadata, 256> records{};
        for (uint32_t i = 0; i < plans.size(); ++i) {
            plans[i] = {1000 + i, 0x1000u + i, static_cast<uint16_t>(i * 16), 16, 24};
            for (uint32_t j = 0; j < 16; ++j)
                names[i * 16 + j] = {uint64_t(i * 16 + j + 1), static_cast<uint8_t>(j), {}};
            records[i] = {plans[i].key, plans[i].mask, 0x12345678,
                          reinterpret_cast<uint64_t>(&names[i * 16]), 16, 16};
        }
        const auto beforeNames = names;
        const auto beforeRecords = records;
        require(table->configure(plans, names), "maximum table configuration");
        require(!table->configure(plans, names), "live reconfiguration rejected");
        std::atomic<uint32_t> failures{0}, ready{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        for (uint32_t worker = 0; worker < 8; ++worker)
            threads.emplace_back([&, worker] {
                ++ready;
                while (!start.load()) std::this_thread::yield();
                for (uint32_t round = 0; round < 4; ++round)
                    for (uint32_t n = 0; n < 256; ++n) {
                        const uint32_t i = (n + worker * 31) % 256;
                        const auto result = table->rewrite(plans[i].key, &records[i], readOwned);
                        const auto* m = result.metadata;
                        if (!m || m->key != plans[i].key || m->mask != (plans[i].mask | 128) ||
                            m->count != 17 || m->capacity != 17 || m->opaque != records[i].opaque) {
                            ++failures;
                            continue;
                        }
                        const auto* added = reinterpret_cast<const Table::Name*>(m->names);
                        if (added[16].hash != Table::MotionName || added[16].index != 24) ++failures;
                        for (uint32_t j = 0; j < 16; ++j)
                            if (added[j].hash != names[i * 16 + j].hash || added[j].index != j) ++failures;
                    }
            });
        while (ready.load() != 8) std::this_thread::yield();
        start = true;
        for (auto& thread : threads) thread.join();
        require(!failures.load(), "concurrent initialization and immutable reuse");
        require(!std::memcmp(names.data(), beforeNames.data(), sizeof(names)), "original names unchanged");
        require(!std::memcmp(records.data(), beforeRecords.data(), sizeof(records)), "original records unchanged");
        auto relocated = records[0];
        std::array<Table::Name, 16> reordered{};
        std::copy_n(names.begin(), 16, reordered.begin());
        std::reverse(reordered.begin(), reordered.end());
        relocated.names = reinterpret_cast<uint64_t>(reordered.data());
        require(table->rewrite(plans[0].key, &relocated, readOwned).state == Table::State::Revalidated,
                "relocated reordered declaration");
        names[0].index ^= 1;
        require(!table->rewrite(plans[0].key, &records[0], readOwned).metadata,
                "same-address changed input rejected");
        names = beforeNames;
        relocated.mask ^= 0x80000000u;
        require(!table->rewrite(plans[0].key, &relocated, readOwned).metadata, "changed request mask rejected");
        require(!table->rewrite(plans[0].key, nullptr, readOwned).metadata, "unreadable source rejected");
        require(!table->rewrite(0xffffffffffffffffULL, nullptr, readOwned).metadata, "unknown key untouched");
        auto bad = std::make_unique<Table>();
        auto invalidPlans = plans;
        invalidPlans[1].key = invalidPlans[0].key;
        require(!bad->configure(invalidPlans, names), "duplicate key rejected");
        invalidPlans = plans; invalidPlans[0].motionRow = 25;
        require(!bad->configure(invalidPlans, names), "four-row overflow rejected");
        invalidPlans = plans; invalidPlans[1].nameOffset = 0;
        require(!bad->configure(invalidPlans, names), "overlapping plan names rejected");
        auto invalidNames = names; invalidNames[0].hash = Table::MotionName;
        require(!bad->configure(plans, invalidNames), "existing motion declaration rejected");
        std::printf("{\"table_bytes\":%zu,\"declarations\":256,\"names\":4096,\"concurrent_calls\":8192,"
                    "\"original_unchanged\":true,\"relocation_revalidated\":true,\"invalid_rejected\":true,"
                    "\"game_attached\":false}\n", sizeof(Table));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
