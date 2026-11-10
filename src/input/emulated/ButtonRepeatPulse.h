#pragma once

#include <chrono>
#include <cstdint>

namespace input
{
template<class Clock>
bool ButtonRepeatPulse(uint32_t held, uint32_t previous, uint32_t repeatFlag,
    int32_t delayMs, int32_t pulseMs, typename Clock::time_point now,
    typename Clock::time_point& changed, typename Clock::time_point& lastPulse)
{
    // The repeat flag is output metadata, not another held button.
    held &= ~repeatFlag;
    previous &= ~repeatFlag;
    if (!held || held != previous)
    {
        changed = lastPulse = now;
        return false;
    }
    if (pulseMs <= 0 || delayMs < 0 ||
        now < changed + std::chrono::milliseconds(delayMs) ||
        now < lastPulse + std::chrono::milliseconds(pulseMs))
        return false;
    lastPulse = now;
    return true;
}
}
