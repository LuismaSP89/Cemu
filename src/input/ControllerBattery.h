#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

namespace ControllerBattery
{
    constexpr std::optional<std::uint8_t> Percentage(int percent)
    {
        if (percent < 0 || percent > 100) return std::nullopt;
        return static_cast<std::uint8_t>(percent);
    }

    // VPAD exposes native battery steps, not an arbitrary percentage.
    constexpr std::uint8_t VpadLevel(std::optional<std::uint8_t> percent)
    {
        if (!percent) return 0xc0; // Preserve the existing no-telemetry fallback.
        return static_cast<std::uint8_t>(std::min(4u, (static_cast<unsigned>(*percent) + 24u) / 25u));
    }
}
