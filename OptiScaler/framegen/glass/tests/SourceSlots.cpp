#include "../GeometrySourceSlots.h"
#include <iostream>
#include <stdexcept>

void require(bool value) { if (!value) throw std::runtime_error("Source slot provenance failed"); }
int main()
{
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
    std::cout << "PASS reorder=1 stale_submission=1 ambiguity=1 generation_passthrough=1 "
                 "allocation=0 scans_per_lookup=0 live_engine=0 motion_produced=0\n";
}
