#define GLASS_ARRAY_WRAPPER
#include "../ExperimentInstanceUpdates.cpp"
#include <iostream>
#include <stdexcept>

unsigned forwards = 0;
void* forwarded[3] {};
unsigned char forward(void* a, void* b, void* c)
{
    ++forwards; forwarded[0] = a; forwarded[1] = b; forwarded[2] = c; return 39;
}
void require(bool value) { if (!value) throw std::runtime_error("Array wrapper observer failed"); }
int main()
{
    original = forward;
    std::array<std::uint64_t, 3> handle { 1, 2, 0x123456 };
    // Intentionally unmapped: the array itself must never be read.
    std::array<std::uint64_t, 2> span { 0x40000, 0x40090 };
    std::array<std::uint64_t, 4> bounds { 4, 5, 6, 7 };
    auto call = [&] { return enqueue(handle.data(), span.data(), bounds.data()); };
    require(call() == 39 && used == 0);
    recording = true; arraysOnly = true;
    require(call() == 39 && used == 1 && active == 0);
    require(rows[0].valid && rows[0].caller && rows[0].header[0] == handle[2]);
    require(rows[0].context == reinterpret_cast<std::uint64_t>(handle.data()));
    require(rows[0].input == reinterpret_cast<std::uint64_t>(span.data()));
    for (unsigned i = 0; i < 4; ++i) require(rows[0].header[8 + i] == bounds[i]);
    require(rows[0].header[12] == span[0] && rows[0].header[13] == span[1]);
    require(forwarded[0] == handle.data() && forwarded[1] == span.data() && forwarded[2] == bounds.data());
    span[1] = span[0];
    for (unsigned i = 0; i < 5000; ++i) require(call() == 39);
    require(used == 1 && filtered == 5000 && recording);
    span[1] = span[0] + 47;
    require(call() == 39 && malformed == 1 && used == 1);
    require(enqueue(handle.data(), nullptr, bounds.data()) == 39 && malformed == 2);
    require(enqueue(nullptr, span.data(), bounds.data()) == 39 && malformed == 3);
    require(enqueue(handle.data(), span.data(), nullptr) == 39 && malformed == 4);
    span[1] = span[0] + 48;
    used = unsigned(rows.size());
    require(call() == 39 && !recording && active == 0 && forwards == 5007);
    require(profileMagic == 0x49555032);
    std::cout << "PASS wrapper_arguments=1 upstream_caller=1 normalized_header=1 "
                 "span_not_dereferenced=1 bounded_filter=1 return_preserved=1 hooks_installed=0\n";
}
