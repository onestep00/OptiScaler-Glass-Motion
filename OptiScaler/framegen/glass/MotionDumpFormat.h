#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace GlassFg
{
// Dump-only motion encoder. The PPM visualization quantizes motion to 1/512 and
// clamps it to +-0.25, so a corrected pixel and an uncorrected one can encode to
// the same byte. This writer keeps the exact half pairs as little-endian RG32F
// behind a one-line text header, so a dump can be compared numerically.
//
// The source texture is R16G16B16A16_FLOAT, so one pixel is four halves: the
// first two are the motion pair and the rest are ignored here.
inline float HalfMotionToFloat(std::uint16_t value) noexcept
{
    const auto sign = std::uint32_t(value & 0x8000u) << 16;
    auto exponent = std::uint32_t((value >> 10) & 0x1fu);
    auto mantissa = std::uint32_t(value & 0x3ffu);
    std::uint32_t bits = 0;
    if (!exponent)
    {
        if (!mantissa)
            bits = sign;
        else
        {
            auto adjusted = 127 - 15 + 1;
            while (!(mantissa & 0x400u))
            {
                mantissa <<= 1;
                --adjusted;
            }
            bits = sign | (std::uint32_t(adjusted) << 23) | ((mantissa & 0x3ffu) << 13);
        }
    }
    else if (exponent == 31)
        bits = sign | 0x7f800000u | (mantissa << 13);
    else
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    float result = 0.f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

// rows: first row of the readback image. rowStrideBytes is the footprint's
// RowPitch, which can be larger than the row, so the padding is skipped instead
// of being written into the file.
inline bool WriteMotion32F(const wchar_t* path, const std::byte* rows, unsigned width, unsigned height,
                           std::uint64_t rowStrideBytes, unsigned dumpSerial, unsigned frame) noexcept
{
    const auto rowBytes = std::uint64_t(width) * 8u;
    if (!path || !rows || !width || !height || rowStrideBytes < rowBytes)
        return false;
    FILE* file = _wfopen(path, L"wb");
    if (!file)
        return false;
    std::fprintf(file, "MV32 %u %u serial=%u frame=%u\n", width, height, dumpSerial, frame);
    for (unsigned y = 0; y < height; ++y)
    {
        const auto* row = reinterpret_cast<const std::uint16_t*>(rows + std::uint64_t(y) * rowStrideBytes);
        for (unsigned x = 0; x < width; ++x)
        {
            const float pair[2] { HalfMotionToFloat(row[x * 4 + 0]), HalfMotionToFloat(row[x * 4 + 1]) };
            if (std::fwrite(pair, sizeof(float), 2, file) != 2)
            {
                std::fclose(file);
                return false;
            }
        }
    }
    return std::fclose(file) == 0;
}
} // namespace GlassFg
