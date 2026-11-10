#pragma once
#include <algorithm>
#include <cstdint>
namespace WiimoteBattery
{
    constexpr std::uint8_t Percent(std::uint8_t raw)
    {
        return static_cast<std::uint8_t>(std::min<unsigned>(raw, 0xc0) * 100 / 0xc0);
    }
    constexpr std::uint8_t Bars(std::uint8_t percent)
    {
        return static_cast<std::uint8_t>(std::min<unsigned>(4, (percent + 24) / 25));
    }
}
