#define GLASS_TRANSFORM_RANGE
#include "../ExperimentInstanceUpdates.cpp"
#include <iostream>
#include <stdexcept>

unsigned forwards = 0;
void* forwarded[2] {};
void* forward(void* a, void* b)
{
    ++forwards; forwarded[0] = a; forwarded[1] = b;
    auto* out = static_cast<std::uint64_t*>(b);
    if (out) { out[0] = 0x40090; out[1] = 0x400f0; }
    return b;
}
void require(bool value) { if (!value) throw std::runtime_error("Transform range observer failed"); }
int main()
{
    original = forward;
    std::array<std::uint64_t, 7> buffer {};
    buffer[6] = 0x40000; // Intentionally unmapped transform data.
    std::array<std::uint64_t, 3> range {
        reinterpret_cast<std::uint64_t>(buffer.data()), 0x123456, (2ULL << 32) | 3 };
    std::array<std::uint64_t, 2> out {};
    require(enqueue(range.data(), out.data()) == out.data() && used == 0);
    recording = true; arraysOnly = true;
    require(enqueue(range.data(), out.data()) == out.data() && used == 1);
    require(rows[0].valid && rows[0].caller && active == 0);
    for (unsigned i = 0; i < 3; ++i) require(rows[0].header[i] == range[i]);
    require(rows[0].header[3] == buffer[6]);
    require(rows[0].header[12] == out[0] && rows[0].header[13] == out[1]);
    require(forwarded[0] == range.data() && forwarded[1] == out.data());
    require(enqueue(nullptr, out.data()) == out.data() && used == 1 && malformed == 1);
    require(enqueue(range.data(), nullptr) == nullptr && used == 1 && malformed == 2);
    range[0] = 0x7fffffffffffffffULL;
    require(enqueue(range.data(), out.data()) == out.data() && malformed == 3);
    range[0] = reinterpret_cast<std::uint64_t>(buffer.data());
    used = unsigned(rows.size());
    require(enqueue(range.data(), out.data()) == out.data() && !recording && active == 0);
    require(forwards == 6 && profileMagic == 0x49555033);
    std::cout << "PASS pointer_return=1 original_output=1 source_range=1 "
                 "unreadable_rejected=1 data_not_dereferenced=1 bounded_stop=1 hooks_installed=0\n";
}
