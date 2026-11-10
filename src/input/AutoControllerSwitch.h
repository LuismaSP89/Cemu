#pragma once
#include <cstdint>

namespace AutoControllerSwitch
{
    constexpr bool Enabled(bool configured, bool requested, bool helper)
    {
        return !helper && (configured || requested);
    }
    enum class Kind { None, Pro, Wiimote };

    constexpr Kind Select(Kind current, bool proConnected, bool wiimoteConnected)
    {
        if (current == Kind::Pro && proConnected) return current;
        if (current == Kind::Wiimote && wiimoteConnected) return current;
        if (proConnected) return Kind::Pro;
        return wiimoteConnected ? Kind::Wiimote : Kind::None;
    }

    struct Handoff
    {
        std::uintptr_t pending{};
        std::uint64_t after{};
        void Reset() { pending = 0; after = 0; }
        bool Ready(std::uintptr_t current, std::uintptr_t target, std::uint64_t now)
        {
            if (!target) { Reset(); return false; }
            if (!current || current == target) { Reset(); return true; }
            if (pending != target) { pending = target; after = now + 100; }
            return now >= after;
        }
    };
}
