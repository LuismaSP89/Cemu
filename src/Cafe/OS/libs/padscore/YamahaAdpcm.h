#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
namespace padscore
{
    struct YamahaAdpcm
    {
        std::int32_t predictor = 0, step = 127;
        std::int32_t Decode(unsigned nibble) const
        {
            const int delta = step * (2 * static_cast<int>(nibble & 7) + 1) / 8;
            return std::clamp(predictor + ((nibble & 8) ? -delta : delta), -32768, 32767);
        }
        std::uint8_t Encode(std::int16_t sample)
        {
            constexpr std::array<int, 8> factors{230,230,230,230,307,409,512,614};
            step = std::clamp(step, 127, 24576); predictor = std::clamp(predictor, -32768, 32767);
            unsigned best = 0; int error = 65536;
            for (unsigned nibble = 0; nibble < 16; ++nibble)
            {
                const auto distance = std::abs(static_cast<int>(sample) - Decode(nibble));
                if (distance < error) { error = distance; best = nibble; }
            }
            predictor = Decode(best); step = std::clamp(step * factors[best & 7] / 256, 127, 24576);
            return static_cast<std::uint8_t>(best);
        }
    };
}
