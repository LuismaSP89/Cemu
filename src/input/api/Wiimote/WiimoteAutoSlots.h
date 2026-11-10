#pragma once
#include <cstddef>
#include <optional>
#include <span>

namespace WiimoteAutoSlots
{
    inline std::optional<std::size_t> FirstFree(std::span<const bool> occupied)
    {
        for (std::size_t slot = 0; slot < occupied.size(); ++slot)
            if (!occupied[slot]) return slot;
        return std::nullopt;
    }
}
