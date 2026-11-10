#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace WiimoteLifecycle
{
using Clock = std::chrono::steady_clock;
inline bool IsLive(Clock::time_point last, Clock::time_point now)
{
    return last != Clock::time_point{} && now - last < std::chrono::seconds(3);
}
inline bool ValidReport(std::uint8_t id, std::size_t size)
{
    switch (id)
    {
    case 0x20: return size >= 7;
    case 0x21: return size >= 22;
    case 0x22: return size >= 5;
    case 0x30: return size >= 3;
    case 0x31: return size >= 6;
    case 0x32: return size >= 11;
    case 0x33: return size >= 18;
    case 0x34: case 0x35: case 0x36: case 0x37: case 0x3d: return size >= 22;
    default: return false;
    }
}
inline bool IsMotionPlusId(std::uint32_t id)
{
    // Inactive A6 IDs can retain the last passthrough mode just like active A4 IDs.
    const auto normalized = id & ~0x00000700u;
    return normalized == 0xa4200005u || normalized == 0xa6200005u;
}
// Active MotionPlus stores the passthrough identifier at F6, F8 and F9.
inline unsigned PassthroughType(const std::uint8_t* id)
{
    if (id[2] == 0 && id[0] == 0 && id[3] == 0) return 1;
    if ((id[2] == 0 || id[2] == 1) && id[0] == 1 && id[3] == 1) return 2;
    return 0;
}

}
