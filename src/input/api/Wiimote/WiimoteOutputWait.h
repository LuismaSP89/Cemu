#pragma once
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

class WiimoteOutputWait
{
public:
    WiimoteOutputWait()
    {
#ifdef _WIN32
        // Local to the HID writer; never change the process/system timer period.
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
#endif
    }
    ~WiimoteOutputWait()
    {
#ifdef _WIN32
        if (m_timer) CloseHandle(m_timer);
#endif
    }
    WiimoteOutputWait(const WiimoteOutputWait&) = delete;
    WiimoteOutputWait& operator=(const WiimoteOutputWait&) = delete;
    void Wait(bool streaming)
    {
        WaitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(1), streaming);
    }
    void WaitUntil(std::chrono::steady_clock::time_point deadline, bool streaming)
    {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) return;
#ifdef _WIN32
        if (streaming && m_timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() + 99) / 100;
            const auto timeout = static_cast<DWORD>(std::clamp<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() + 11, 10, 1000));
            if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE) &&
                WaitForSingleObject(m_timer, timeout) == WAIT_OBJECT_0) return;
        }
#endif
        std::this_thread::sleep_until(deadline);
    }
private:
#ifdef _WIN32
    HANDLE m_timer{};
#endif
};
