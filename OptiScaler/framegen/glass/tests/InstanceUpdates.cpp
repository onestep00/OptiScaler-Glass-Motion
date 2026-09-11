#include "../ExperimentInstanceUpdates.cpp"
#include <iostream>
#include <stdexcept>
unsigned forwards = 0;
unsigned char forward(void*, void*) { ++forwards; return 27; }
void require(bool value) { if (!value) throw std::runtime_error("Update observer failed"); }
int main()
{
    original = forward;
    std::array<std::uint64_t, 18> data {};
    data[0] = 0x12345; data[12] = 0x40000; data[13] = 0x40060;
    require(enqueue(nullptr, data.data()) == 27 && used == 0);
    recording = true;
    require(enqueue(nullptr, data.data()) == 27 && used == 1 && active == 0);
    require(rows[0].valid && rows[0].header == data);
    require(enqueue(nullptr, nullptr) == 27 && !rows[1].valid);
    used = unsigned(rows.size());
    require(enqueue(nullptr, data.data()) == 27 && !recording && active == 0);
    require(forwards == 4);
    used = 0; arraysOnly = true; recording = true;
    auto empty = data; empty[12] = empty[13] = 0;
    for (unsigned i = 0; i < 5000; ++i) require(enqueue(nullptr, empty.data()) == 27);
    require(used == 0 && recording && filtered == 5000);
    auto invalid = data; invalid[13] = invalid[12] - 48;
    require(enqueue(nullptr, invalid.data()) == 27 && used == 0 && malformed == 1);
    invalid[13] = invalid[12] + 47;
    require(enqueue(nullptr, invalid.data()) == 27 && used == 0 && malformed == 2);
    require(enqueue(nullptr, nullptr) == 27 && used == 0 && malformed == 3);
    // Deliberately unmapped span: the diagnostic must only copy its header.
    require(enqueue(nullptr, data.data()) == 27 && used == 1 && rows[0].header == data);
    require(forwards == 5008 && active == 0);
    recording = false;
    std::cout << "PASS header=1 missing=1 bounded_stop=1 return_preserved=1 "
                 "array_filter=1 empty_does_not_exhaust=1 malformed_rejected=1 span_not_dereferenced=1 hooks_installed=0\n";
}
