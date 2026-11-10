#pragma once
#include "WiimoteSpeaker.h"
#include <chrono>
#include <type_traits>

namespace WiimoteOutputSchedule
{
    class Wake
    {
    public:
        using Clock = std::chrono::steady_clock;
        bool AudioReady(Clock::time_point deadline, Clock::time_point now)
        {
            if (now >= deadline) return true;
            m_deadline = std::min(m_deadline, deadline);
            return false;
        }
        void PollControl(Clock::time_point now)
        {
            m_deadline = std::min(m_deadline, now + std::chrono::milliseconds(1));
        }
        Clock::time_point Deadline(Clock::time_point now) const
        {
            return m_deadline == Clock::time_point::max() ? now + std::chrono::milliseconds(1) : m_deadline;
        }
    private:
        Clock::time_point m_deadline = Clock::time_point::max();
    };

    constexpr bool IsAudio(std::uint8_t report) { return report == 0x18; }
    constexpr bool AudioMayPass(std::uint8_t report)
    {
        // These carry no speaker configuration or stream-state changes.
        return report == 0x10 || report == 0x11 || report == 0x15;
    }

    template<class Queue, class Ready>
    auto Select(Queue& queue, std::size_t devices, Ready ready)
    {
        std::vector<std::uint8_t> blocked(devices);
        auto fallback = queue.end();
        for (auto it = queue.begin(); it != queue.end(); ++it)
        {
            const auto index = it->first;
            if (index >= devices || blocked[index] == 2 || it->second.empty()) continue;
            const auto report = it->second[0];
            if (blocked[index] == 1 && !IsAudio(report))
            {
                if (!AudioMayPass(report)) blocked[index] = 2;
                continue;
            }
            const bool isReady = [&] {
                if constexpr (std::is_invocable_v<Ready,std::size_t,std::uint8_t,const typename Queue::value_type&>) return ready(index,report,*it);
                else return ready(index,report);
            }();
            if (isReady)
            {
                if (IsAudio(report)) return it;
                if (fallback == queue.end()) fallback = it;
            }
            // Only harmless controls may yield to audio on the same device.
            blocked[index] = AudioMayPass(report) ? 1 : 2;
        }
        return fallback;
    }

    class AudioClock
    {
    public:
        using Clock = std::chrono::steady_clock;
        AudioClock() = default;
        explicit AudioClock(Clock::time_point start) : m_samples(start), m_ready(start) {}
        Clock::time_point Deadline() const { return m_ready; }
        void SetCompatibility(bool enabled) { m_rate=enabled?3000:6000; }
        void NotBefore(Clock::time_point time) { m_ready=std::max(m_ready,time); }
        void Sent(Clock::time_point submission, unsigned bytes)
        {
            const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(std::int64_t(bytes)*2*1000000000/m_rate));
            if (submission > m_samples + std::chrono::milliseconds(20)) m_samples = submission;
            m_samples += period;
            const auto minimumGap = std::min(period, std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(5)));
            // A temporary spacing guard must not permanently shift the sample clock.
            m_ready = std::max(m_samples, submission + minimumGap);
        }
    private:
        Clock::time_point m_samples{}, m_ready{};
        unsigned m_rate=6000;
    };

    template<class ControlTime>
    void Sent(std::uint8_t report, unsigned bytes, ControlTime controlNow,
              std::chrono::steady_clock::time_point audioSubmission, ControlTime& lastControl,
              AudioClock& nextAudio)
    {
        if (IsAudio(report)) nextAudio.Sent(audioSubmission, bytes);
        else lastControl = controlNow;
    }
}
