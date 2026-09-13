#include "../MotionShaderScope.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        using Scope = GlassFg::MotionShaderScope;
        Scope table;
        std::vector<Scope::Pair> pairs;
        for (uint64_t i = 0; i < Scope::MaxPairs; ++i)
            pairs.push_back({ 100 + i, 100000 + i, 1 + i % 83 });
        require(table.configure(pairs), "full bounded pair table");
        require(!table.configure(pairs), "immutable configuration");
        for (const auto& p : pairs) {
            require(table.lookup(p.vertex, p.partner) == p.metadata, "exact VS/PS pair");
            require(!table.lookup(p.partner, p.vertex), "pixel stage must not use vertex declaration");
            require(!table.lookup(p.vertex, p.partner + 1), "different pixel partner");
        }
        require(!table.lookup(0, 0), "missing shader pair");
        require(!Scope::Invocation::allows(7, 123), "unscoped metadata call");
        {
            Scope::Invocation outer(7, 123);
            require(Scope::Invocation::allows(7, 123), "selected direct call");
            require(!Scope::Invocation::allows(7, 124), "unrelated call site");
            require(!Scope::Invocation::allows(8, 123), "unrelated metadata key");
            {
                Scope::Invocation unsupported(0, 123);
                require(!Scope::Invocation::allows(7, 123), "nested unsupported stage");
            }
            require(Scope::Invocation::allows(7, 123), "outer selection restored");
            try { Scope::Invocation other(8, 321); throw 1; } catch (int) {}
            require(Scope::Invocation::allows(7, 123), "exception scope restored");
        }
        std::atomic<unsigned> completed = 0;
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 8; ++i) threads.emplace_back([&, i] {
            for (unsigned j = 0; j < 4096; ++j) {
                Scope::Invocation scope(i + 1, 123);
                if (Scope::Invocation::allows(i + 1, 123) && !Scope::Invocation::allows(i + 2, 123)) ++completed;
            }
        });
        for (auto& t : threads) t.join();
        require(completed == 32768 && !Scope::Invocation::allows(1, 123), "thread isolation");
        Scope invalid;
        auto bad = pairs; bad[1] = bad[0];
        require(!invalid.configure(bad), "duplicate/conflicting pair");
        bad = pairs; bad[0].metadata = 0;
        require(!invalid.configure(bad), "missing declaration");
        std::printf("{\"pairs\":4096,\"thread_calls\":32768,\"nested_scope\":true,"
                    "\"wrong_stage_partner_caller_rejected\":true,\"table_bytes\":%zu,\"game_attached\":false}\n", sizeof(Scope));
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
