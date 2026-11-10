#pragma once
#include <cstdint>

namespace WpadLifecycle
{
inline bool ValidMode(std::uint8_t mode) { return mode == 4 || mode == 5 || mode == 7; }
inline std::uint8_t MotionPlusStatus(std::uint8_t mode, bool ready, unsigned physical_type)
{
    if (!ready || !ValidMode(mode)) return 0;
    if (physical_type < 5 || physical_type > 7) return 0;
    // The native provider switches passthrough on physical hotplug. Report its
    // active extension, not the application's last requested pre-hotplug mode.
    if (physical_type == 6) return 5;
    if (physical_type == 7) return 7;
    return 4;
}
struct Connection
{
    bool known = false;
    bool connected = false;
    unsigned extension = 253;
    bool Update(bool live)
    {
        const bool changed = !known || connected != live;
        known = true;
        connected = live;
        if (changed) extension = 253;
        return changed;
    }
};
}
