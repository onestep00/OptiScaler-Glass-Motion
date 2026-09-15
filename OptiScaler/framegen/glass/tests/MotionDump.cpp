#include "../MotionDumpFormat.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

std::uint32_t bits(float value)
{
    std::uint32_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

std::vector<std::byte> readFile(const wchar_t* path)
{
    FILE* file = _wfopen(path, L"rb");
    if (!file)
        throw std::runtime_error("dump file was not written");
    std::vector<std::byte> data;
    std::byte buffer[4096];
    for (;;)
    {
        const auto read = std::fread(buffer, 1, sizeof(buffer), file);
        data.insert(data.end(), buffer, buffer + read);
        if (read != sizeof(buffer))
            break;
    }
    std::fclose(file);
    return data;
}

bool readFileExists(const wchar_t* path)
{
    FILE* file = _wfopen(path, L"rb");
    if (!file)
        return false;
    std::fclose(file);
    return true;
}
} // namespace

int main()
{
    try
    {
        // Exact half -> float conversion, pinned by bit pattern. The subnormal
        // pair matters because a camera-continuous MV often lands there.
        const struct
        {
            std::uint16_t half;
            std::uint32_t bits;
        } table[] {
            { 0x0000u, 0x00000000u }, { 0x8000u, 0x80000000u }, { 0x3C00u, 0x3F800000u }, { 0xBC00u, 0xBF800000u },
            { 0x3800u, 0x3F000000u }, { 0xB800u, 0xBF000000u }, { 0x3400u, 0x3E800000u }, { 0x3E00u, 0x3FC00000u },
            { 0x7C00u, 0x7F800000u }, { 0xFC00u, 0xFF800000u }, { 0x7E00u, 0x7FC00000u }, { 0x0001u, 0x33800000u },
            { 0x03FFu, 0x387FC000u }, { 0x0400u, 0x38800000u }, { 0x7BFFu, 0x477FE000u }, { 0xFBFFu, 0xC77FE000u },
        };
        for (const auto& entry : table)
            check(bits(GlassFg::HalfMotionToFloat(entry.half)) == entry.bits, "half conversion mismatch");

        constexpr unsigned kWidth = 4, kHeight = 3;
        constexpr std::uint64_t kStride = std::uint64_t(kWidth) * 8u + 8u;
        const std::uint16_t pixels[kHeight][kWidth][4] {
            { { 0x0000, 0x3C00, 0x1111, 0x2222 },
              { 0xBC00, 0x7E00, 0x3333, 0x4444 },
              { 0x7C00, 0xFC00, 0x5555, 0x6666 },
              { 0x0001, 0x03FF, 0x7777, 0x8888 } },
            { { 0x0400, 0x3800, 0x9999, 0xAAAA },
              { 0xB800, 0x3400, 0xBBBB, 0xCCCC },
              { 0x3E00, 0x7BFF, 0xDDDD, 0xEEEE },
              { 0xFBFF, 0x8000, 0x1234, 0x5678 } },
            { { 0x3C00, 0x3C00, 0x0F0F, 0xF0F0 },
              { 0x0000, 0x8000, 0x00FF, 0xFF00 },
              { 0x03FF, 0x0400, 0x1E1E, 0xE1E1 },
              { 0x7E00, 0xFC00, 0x2D2D, 0xD2D2 } },
        };
        std::vector<std::byte> image(size_t(kStride) * kHeight, std::byte(0xCD));
        for (unsigned y = 0; y < kHeight; ++y)
            std::memcpy(image.data() + y * kStride, pixels[y], sizeof(pixels[y]));
        const auto source = image;

        const auto* path = L"motion-dump-test.f32";
        _wremove(path);
        check(GlassFg::WriteMotion32F(path, image.data(), kWidth, kHeight, kStride, 7, 99),
              "writer rejected a valid image");
        const auto file = readFile(path);
        // The stride padding must not reach the file: the payload is exactly
        // one RG32F pair per pixel.
        const std::string header = "MV32 4 3 serial=7 frame=99\n";
        check(file.size() == header.size() + size_t(kWidth) * kHeight * 8, "unexpected payload size");
        check(std::memcmp(file.data(), header.data(), header.size()) == 0, "unexpected header");
        size_t offset = header.size();
        for (unsigned y = 0; y < kHeight; ++y)
            for (unsigned x = 0; x < kWidth; ++x)
            {
                const float expected[2] { GlassFg::HalfMotionToFloat(pixels[y][x][0]),
                                          GlassFg::HalfMotionToFloat(pixels[y][x][1]) };
                float actual[2] {};
                std::memcpy(actual, file.data() + offset, sizeof(actual));
                offset += sizeof(actual);
                check(bits(actual[0]) == bits(expected[0]) && bits(actual[1]) == bits(expected[1]),
                      "payload mismatch");
            }
        check(image == source, "writer modified the source image");

        // Invalid input is rejected without producing a file.
        _wremove(path);
        check(!GlassFg::WriteMotion32F(path, image.data(), kWidth, kHeight, std::uint64_t(kWidth) * 8u - 1u, 1, 1),
              "short stride accepted");
        check(!GlassFg::WriteMotion32F(path, image.data(), 0, kHeight, kStride, 1, 1), "zero width accepted");
        check(!GlassFg::WriteMotion32F(path, image.data(), kWidth, 0, kStride, 1, 1), "zero height accepted");
        check(!GlassFg::WriteMotion32F(path, nullptr, kWidth, kHeight, kStride, 1, 1), "null rows accepted");
        check(!GlassFg::WriteMotion32F(nullptr, image.data(), kWidth, kHeight, kStride, 1, 1), "null path accepted");
        check(!readFileExists(path), "rejected write still produced a file");
        _wremove(path);

        std::printf("MOTION_DUMP table=%zu pair_bytes=8 payload_bytes=%zu ok=1\n", sizeof(table) / sizeof(table[0]),
                    size_t(kWidth) * kHeight * 8);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
