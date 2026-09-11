#define GLASS_NODE_GROUP
#include "../ExperimentInstanceUpdates.cpp"
#include <iostream>
#include <stdexcept>

std::array<std::uint64_t, 32> instance {}, node {}, shared {};
std::array<std::uint64_t, 8> handles {};
std::array<std::uint64_t, 3> rendererHandle {1, 2, 0x123456};
std::array<std::uint64_t, 2> span {0x40000 + 107 * 48, 0x40000 + 120 * 48};
std::array<std::uint64_t, 4> bounds {};
bool append = true;
unsigned forwards = 0;
void require(bool value) { if (!value) throw std::runtime_error("Node group observation failed"); }
void forward(void* a, float distance, void* b, void* c)
{
    require(a == instance.data() && distance == 3.25f && b == span.data() && c == bounds.data());
    ++forwards;
    if (append)
    {
        auto count = instance[0xf0 / 8] >> 32;
        handles[count * 2] = reinterpret_cast<std::uint64_t>(rendererHandle.data());
        instance[0xf0 / 8] = 4 | ((count + 1) << 32);
    }
}
int main()
{
    original = forward;
    instance[0x60 / 8] = reinterpret_cast<std::uint64_t>(node.data());
    instance[0x68 / 8] = 17;
    instance[0xb8 / 8] = 0x234567;
    instance[0xe8 / 8] = reinterpret_cast<std::uint64_t>(handles.data());
    instance[0xf0 / 8] = 4;
    node[0x38 / 8] = reinterpret_cast<std::uint64_t>(shared.data());
    node[0x40 / 8] = 19;
    node[0x48 / 8] = 100 | (std::uint64_t(40) << 32);
    shared[0x30 / 8] = 0x40000; // Intentionally unmapped transform storage.
    shared[0x38 / 8] = 200 * 48;
    auto call = [&] { enqueue(instance.data(), 3.25f, span.data(), bounds.data()); };
    recording = true;
    call();
    require(used == 1 && rows[0].valid && active == 0);
    require(rows[0].context == reinterpret_cast<std::uint64_t>(instance.data()));
    require(rows[0].header[10] == rendererHandle[2] && rows[0].header[11] == 0);
    require(rows[0].header[8] == (107 | (std::uint64_t(13) << 32)));
    append = false; call();
    require(used == 1 && malformed == 1); // rejected group creates no association
    append = true;
    span = {0x40000 + 100 * 48, 0x40000 + 107 * 48}; call();
    require(used == 2 && rows[1].header[11] == 1 &&
            rows[1].header[8] == (100 | (std::uint64_t(7) << 32)));
    shared[0x38 / 8] = 139 * 48; call();
    require(used == 2 && malformed == 2);
    recording = false; append = false; call();
    require(used == 2 && active == 0 && forwards == 5);
    require(profileMagic == 0x49555034);
    std::cout << "PASS mixed_float_abi=1 skipped_group=1 exact_source_span=1 "
                 "no_transform_reads=1 rejected_parent=1 hooks_installed=0 live_engine=0\n";
}
