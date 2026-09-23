#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
namespace Glass {
inline bool WritePackedRecord64(const wchar_t* path, const std::byte* data,
                                std::size_t available, unsigned width, unsigned height,
                                unsigned serial, unsigned frame) {
    if (!path || !data || !width || !height) return false;
    const std::uint64_t bytes = std::uint64_t(width) * height * 8;
    if (bytes / 8 / width != height || bytes > (std::numeric_limits<std::size_t>::max)() || bytes > available)
        return false;
    FILE* file = _wfopen(path, L"wb");
    if (!file) return false;
    const bool header = std::fprintf(file, "PACKREC64 %u %u serial=%u frame=%u bytes=%llu\n",
        width, height, serial, frame, static_cast<unsigned long long>(bytes)) > 0;
    const bool payload = header && std::fwrite(data, 1, static_cast<std::size_t>(bytes), file) == bytes;
    const bool closed = std::fclose(file) == 0;
    return header && payload && closed;
}
}
