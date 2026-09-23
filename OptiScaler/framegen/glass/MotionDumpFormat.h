#pragma once
#include <dxgiformat.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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

// Colour encodings WriteColorPpm converts. Every other format is written as
// mid-grey, so a caller that cannot use such a file checks this first.
inline bool ColorPpmSupported(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

// 8-bit PPM of one colour readback. rows is the first row of the placed
// footprint, rowStrideBytes its RowPitch. Half floats are scaled by 255 and
// clamped, 10-bit channels drop their two low bits; no tone mapping.
inline bool WriteColorPpm(const wchar_t* path, const std::byte* rows, DXGI_FORMAT format, unsigned width,
                          unsigned height, std::uint64_t rowStrideBytes) noexcept
{
    if (!path || !rows || !width || !height)
        return false;
    FILE* file = _wfopen(path, L"wb");
    if (!file)
        return false;
    const auto toByte = [](float value) { return static_cast<std::byte>(std::clamp(value, 0.f, 255.f)); };
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    bool okay = true;
    try
    {
        std::vector<std::byte> line(std::size_t(width) * 3u);
        for (unsigned y = 0; y < height && okay; ++y)
        {
            const auto* row = rows + std::uint64_t(y) * rowStrideBytes;
            for (unsigned x = 0; x < width; ++x)
            {
                std::byte* pixel = line.data() + std::size_t(x) * 3u;
                pixel[0] = pixel[1] = pixel[2] = std::byte(128);
                switch (format)
                {
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                    pixel[0] = row[x * 4 + 0];
                    pixel[1] = row[x * 4 + 1];
                    pixel[2] = row[x * 4 + 2];
                    break;
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                    pixel[0] = row[x * 4 + 2];
                    pixel[1] = row[x * 4 + 1];
                    pixel[2] = row[x * 4 + 0];
                    break;
                case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                {
                    std::uint32_t value = 0;
                    std::memcpy(&value, row + x * 4, sizeof(value));
                    pixel[0] = std::byte((value & 0x3ffu) >> 2);
                    pixel[1] = std::byte(((value >> 10) & 0x3ffu) >> 2);
                    pixel[2] = std::byte(((value >> 20) & 0x3ffu) >> 2);
                    break;
                }
                case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                case DXGI_FORMAT_R16G16B16A16_FLOAT:
                {
                    std::uint16_t half[3] {};
                    std::memcpy(half, row + std::size_t(x) * 8u, sizeof(half));
                    pixel[0] = toByte(HalfMotionToFloat(half[0]) * 255.f);
                    pixel[1] = toByte(HalfMotionToFloat(half[1]) * 255.f);
                    pixel[2] = toByte(HalfMotionToFloat(half[2]) * 255.f);
                    break;
                }
                default:
                    break;
                }
            }
            okay = std::fwrite(line.data(), 1, line.size(), file) == line.size();
        }
    }
    catch (...)
    {
        okay = false;
    }
    return std::fclose(file) == 0 && okay;
}
} // namespace GlassFg
