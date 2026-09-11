// Independent process and fixture DLL only. Never attaches to a game.
#include "../ExperimentInstanceProducer.cpp"
#include <iostream>
#include <stdexcept>

void require(bool value) { if (!value) throw std::runtime_error("Source query DLL connection failed"); }
int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 2);
        const auto provider = std::filesystem::absolute(argv[1]);
        self = GetModuleHandleW(nullptr);
        wchar_t ownPath[32768];
        require(GetModuleFileNameW(self, ownPath, 32768) != 0);
        auto config = std::filesystem::path(ownPath); config.replace_extension(L".source");
        require(!std::filesystem::exists(config));
        require(connectSource() && !sourceQuery);
        auto configure = [&](const std::filesystem::path& path)
        {
            const auto value = path.u8string();
            std::ofstream file(config, std::ios::binary);
            file.write(reinterpret_cast<const char*>(value.data()), value.size());
            file << '\n'; file.close(); require(bool(file));
        };
        configure("relative.dll");
        require(!connectSource() && !sourceQuery);
        configure(provider);
        require(!GetModuleHandleW(provider.c_str()) && !connectSource() && !sourceQuery);
        const auto module = LoadLibraryW(provider.c_str());
        require(module && connectSource() && sourceQuery);
        using Set = void (*)(std::uint64_t, unsigned);
        const auto set = reinterpret_cast<Set>(GetProcAddress(module, "FixtureSourceSet"));
        require(set != nullptr);
        std::array<unsigned char, 0x118> proxyData {};
        auto put = [&](unsigned offset, const auto& value) { memcpy(proxyData.data() + offset, &value, sizeof(value)); };
        const std::uint64_t proxy = reinterpret_cast<std::uint64_t>(proxyData.data()), mesh = 0x30000;
        const unsigned count = 40, start = 20160;
        put(0xd8, mesh); put(0x110, count); put(0x114, start);
        unsigned frame = 1; tick = &frame;
        Selection::Descriptor descriptor {start, count, 0, 0, 0, 0};
        auto capture = [&]
        {
            Scope scope {proxy, 0, frame++};
            scope.frame = frame;
            current = &scope;
            observe(proxyData.data(), &descriptor, true);
            current = nullptr;
            require(used != 0);
            return rows[used.load() - 1].sourceOwner;
        };
        require(!capture().generation);
        set(proxy, 0);
        const auto first = capture();
        require(first.generation && first.node == 0x40000 && first.buffer == 0x50000 && first.first == 107);
        set(proxy, 1);
        require(!capture().generation);
        set(proxy, 0);
        require(capture().generation > first.generation);
        require(FreeLibrary(module) && GetModuleHandleW(provider.c_str()) == module);
        require(capture().generation != 0); // Pinned callback stays callable.
        require(std::filesystem::remove(config));
        require(connectSource() && !sourceQuery);
        std::cout << "PASS real_dll_export=1 explicit_utf8_path=1 absent_provider_rejected=1 "
                     "source_to_producer=1 destroyed_owner_rejected=1 replacement_generation=1 "
                     "provider_pinned=1 game_hooks_installed=0\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n'; return 1;
    }
}
