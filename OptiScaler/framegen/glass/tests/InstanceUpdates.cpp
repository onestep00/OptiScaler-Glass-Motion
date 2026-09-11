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
    std::cout << "PASS header=1 missing=1 bounded_stop=1 return_preserved=1 hooks_installed=0\n";
}
