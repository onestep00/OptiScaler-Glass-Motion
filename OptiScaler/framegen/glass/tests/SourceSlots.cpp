#include "../GeometrySourceSlots.h"
#include "../GeometrySourceOwners.h"
#include "../VertexHistoryCache.h"
#include <iostream>
#include <stdexcept>

void require(bool value) { if (!value) throw std::runtime_error("Source slot provenance failed"); }
int main()
{
    GlassFg::GeometrySourceOwners<2> owners;
    using OwnerSource = decltype(owners)::Source;
    const OwnerSource source {10, 20, 30, 496, 40};
    const auto creation = owners.created(100, 200, source);
    require(creation && owners.size() == 1);
    // Source can arrive before an engine registry slot exists. Its generation
    // remains distinct from the separately validated registry generation.
    require(owners.find(200, 30).generation == creation);
    require(!owners.find(200, 31));
    require(owners.created(101, 201, source) && owners.size() == 2);
    require(!owners.created(102, 202, source) && owners.find(200, 30));
    owners.destroyed(999, 200); // unrelated owner must not remove this source
    require(bool(owners.find(200, 30)));
    owners.destroyed(100, 200);
    require(!owners.find(200, 30) && owners.size() == 1);
    const auto reused = owners.created(100, 200, source);
    require(reused > creation && owners.find(200, 30).generation == reused);
    owners.destroyed(100, 200);
    owners.destroyed(101, 201);
    require(owners.size() == 0 && !owners.find(201, 30));
    require(!owners.created(0, 200, source));
    require(!owners.created(100, 200, {10, 20, 30, UINT32_MAX, 2}));
    require(owners.created(100, 200, source));
    require(!owners.created(102, 200, {}) && !owners.find(200, 30));
    using Span = GlassFg::GeometrySourceSpan;
    // The first successful renderer is for a later source group. The renderer
    // ordinal is zero, but its source index must remain 107, not zero or 100.
    constexpr std::uint64_t allocation = 0x100000;
    auto later = Span::resolve(allocation, 200 * 48, 100, 40,
                               allocation + 107 * 48, allocation + 120 * 48, 48);
    require(later && later.first == 107 && later.count == 13);
    auto earlier = Span::resolve(allocation, 200 * 48, 100, 40,
                                 allocation + 100 * 48, allocation + 107 * 48, 48);
    require(earlier && earlier.first == 100 && earlier.count == 7);
    require(!Span::resolve(allocation, 200 * 48, 100, 40,
                           allocation + 99 * 48, allocation + 107 * 48, 48));
    require(!Span::resolve(allocation, 200 * 48, 100, 40,
                           allocation + 130 * 48, allocation + 141 * 48, 48));
    require(!Span::resolve(allocation, 139 * 48, 100, 40,
                           allocation + 100 * 48, allocation + 107 * 48, 48));
    require(!Span::resolve(allocation, 200 * 48, 100, 40,
                           allocation + 100 * 48 + 1, allocation + 107 * 48, 48));
    require(!Span::resolve(allocation, 200 * 48, 100, 40, allocation, allocation, 48));
    require(!Span::resolve(allocation, 200 * 48, 100, 40, allocation, allocation + 48, 0));
    require(!Span::resolve(allocation, UINT64_MAX, UINT32_MAX, 2,
                           allocation + std::uint64_t(UINT32_MAX) * 48,
                           allocation + (std::uint64_t(UINT32_MAX) + 1) * 48, 48));
    GlassFg::GeometrySourceSlots<8> slots;
    using Source = decltype(slots)::Source;
    const Source a { 0x10000, 0x20000, 1, 1, 17 }, b { 0x10000, 0x20000, 1, 1, 3 };
    auto first = slots.begin(7, 10);
    require(slots.publish(first, 2, a));
    require(slots.publish(first, 3, b));
    require(!slots.resolve(first, 2)); // incomplete producer domain
    require(slots.seal(first));
    require(slots.resolve(first, 2) == a && slots.resolve(first, 3) == b);
    require(!slots.publish(first, 2, b));
    auto second = slots.begin(7, 11);
    require(slots.publish(second, 2, b) && slots.publish(second, 3, a));
    require(slots.seal(second));
    require(slots.resolve(second, 3) == a && !slots.resolve(first, 2));
    auto third = slots.begin(7, 11); // same frame/view, different submission
    require(!slots.resolve(second, 3));
    require(slots.publish(third, 2, a));
    require(!slots.publish(third, 2, b));
    require(!slots.publish(third, 2, a)); // ambiguity remains sticky
    require(!slots.publish(third, 8, a));
    require(slots.publish(third, 4, a));
    require(!slots.publish(third, 4, {}));
    require(slots.seal(third));
    require(!slots.resolve(third, 2) && !slots.resolve(third, 4) && !slots.resolve(third, 3));
    auto other = third; ++other.view;
    require(!slots.resolve(other, 2));
    auto fourth = slots.begin(8, 12);
    auto replaced = a; ++replaced.ownerGeneration; ++replaced.arrayGeneration;
    require(slots.publish(fourth, 2, replaced) && slots.seal(fourth));
    require(slots.resolve(fourth, 2) == replaced);
    auto invalid = slots.begin(0, 12);
    require(!slots.publish(invalid, 2, a) && !slots.seal(invalid));
    // Actual source-slot -> history-cache lookup, with changing draw ordinals.
    // Fixture generations are supplied explicitly, not derived from pointers.
    GlassFg::VertexHistoryCache<8, 4, 32, 4> history;
    GlassFg::VertexHistoryKey ownerKey { {a.proxy, a.mesh, 23, a.ownerGeneration}, 9, 10, 11, 0, 4 };
    auto ticket = slots.begin(ownerKey.view, 100);
    require(slots.publish(ticket, 0, a) && slots.publish(ticket, 1, b) && slots.seal(ticket));
    require(history.beginFrame(100, 0));
    const auto ha = history.acquire(slots.resolveHistoryKey(ticket, 0, ownerKey, 100), 6, 100);
    const auto hb = history.acquire(slots.resolveHistoryKey(ticket, 1, ownerKey, 100), 6, 100);
    const auto single = history.acquire(ownerKey, 6, 100);
    require(ha && hb && single && ha.base != hb.base && ha.base != single.base && hb.base != single.base);
    ticket = slots.begin(ownerKey.view, 101);
    require(slots.publish(ticket, 0, b) && slots.publish(ticket, 4, a) && slots.seal(ticket));
    require(history.beginFrame(101, 100));
    const auto movedA = history.acquire(slots.resolveHistoryKey(ticket, 4, ownerKey, 101), 6, 101);
    const auto movedB = history.acquire(slots.resolveHistoryKey(ticket, 0, ownerKey, 101), 6, 101);
    require(movedA.base == ha.base && movedA.generation == ha.generation &&
            movedB.base == hb.base && movedB.generation == hb.generation);
    auto changedArray = a; ++changedArray.arrayGeneration;
    ticket = slots.begin(ownerKey.view, 102);
    require(slots.publish(ticket, 4, changedArray) && slots.seal(ticket));
    require(history.beginFrame(102, 100));
    const auto newA = history.acquire(slots.resolveHistoryKey(ticket, 4, ownerKey, 102), 6, 102);
    require(newA && newA.generation != ha.generation && newA.base != ha.base);
    auto wrongOwner = a; ++wrongOwner.ownerGeneration;
    require(!ownerKey.forSource(wrongOwner) && !ownerKey.forSource(Source{}));
    auto wrongMesh = a; ++wrongMesh.mesh;
    require(!ownerKey.forSource(wrongMesh));
    auto arrayZero = a; arrayZero.index = 0;
    require(ownerKey.forSource(arrayZero) != ownerKey); // Element zero is not the whole owner.
    auto missingArrayGeneration = a; missingArrayGeneration.arrayGeneration = 0;
    require(!ownerKey.forSource(missingArrayGeneration));
    require(!slots.resolveHistoryKey(ticket, 4, ownerKey, 101));
    auto wrongView = ownerKey; ++wrongView.view;
    require(!slots.resolveHistoryKey(ticket, 4, wrongView, 102));
    std::cout << "PASS source_to_history=1 reordered_draw_reuses_history=1 array_replacement_separates_history=1 "
                 "owner_generation_checked=1 single_array_domains_separate=1 bounded_lookup="
              << history.stats.maxLookupInspections << "\n";
    std::cout << "PASS source_owner_cancel=1 owner_reuse=1 bounded_owners=1 skipped_source_groups=1 span_bounds=1 reorder=1 stale_submission=1 ambiguity=1 generation_passthrough=1 "
                 "allocation=0 slot_scans_per_lookup=0 live_engine=0 motion_produced=0\n";
}
