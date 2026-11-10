#pragma once
#include "WiimoteSpeaker.h"
#include <atomic>
#include <chrono>
#include <optional>

namespace WiimoteSpeakerProtocol
{
    using Clock = std::chrono::steady_clock;
    inline bool RestoreMode(std::uint8_t& current,std::uint8_t desired,bool forced)
    {
        if(current==desired && !forced) return false;
        current=desired;return true;
    }
    struct State { bool enabled{}, muted{true}, playing{}; };
    struct Step
    {
        std::vector<std::uint8_t> packet;
        std::uint8_t reply{};
    };

    class Operation
    {
    public:
        bool Begin(unsigned command, std::uint8_t volume, State previous, Clock::time_point now,bool compatibility=false)
        {
            if (Pending() || command > 5) return false;
            if (++generation == 0) ++generation;
            m_steps.clear(); m_next = 0; m_waiting = false; m_result.reset(); m_published = false;
            m_deadline = now + std::chrono::seconds(2); m_command = command; m_state = previous;
            if (command == 1 || command == 5)
            {
                Add({0x14,6}); Add({0x19,6});
                // Cafe WPAD uses 0x80 here, not the legacy 0x08 setup value.
                Write(9,{1}); Write(1,{0x80});
                const auto config = WiimoteSpeaker::Configuration(volume,compatibility);
                // Cafe ON unmutes after setup; only PLAY writes the decoder-start register.
                Write(1,config); Add({0x19,2});
                Add({0x15,0},0x20); m_state = {true,false,false};
            }
            else if (command == 0)
            {
                Add({0x19,6}); Write(1,{1}); Write(9,{0}); Add({0x14,2}); Add({0x15,0},0x20);
                m_state = {};
            }
            else if (command == 4)
            {
                if (!previous.enabled) { m_result=-3; return true; }
                Write(8,{1}); m_state.playing=true;
            }
            else
            {
                Add({0x19,static_cast<std::uint8_t>(command == 2 ? 6 : 2)});
                m_state.muted = command == 2; // Amplifier mute must not stop the decoder.
            }
            return true;
        }
        bool Pending() const { return !m_result && !m_steps.empty(); }
        void Poll(Clock::time_point now) { if (Pending() && now >= m_deadline) m_result=-3; }
        bool Ready(std::uint32_t serial, unsigned step, Clock::time_point now)
        {
            Poll(now);
            return Pending() && serial==generation && step==m_next && !m_waiting;
        }
        void Written(std::uint32_t serial, unsigned step, Clock::time_point now)
        {
            if (!Ready(serial,step,now)) return;
            m_waiting=true; m_deadline=now+std::chrono::milliseconds(500);
        }
        void Acknowledge(std::uint8_t report, std::uint8_t error)
        {
            if (!Pending() || !m_waiting || m_steps[m_next].reply != 0x22 || m_steps[m_next].packet[0] != report) return;
            if (error) { m_result=-3; return; }
            Advance();
        }
        void Status(std::uint8_t flags)
        {
            if (!Pending() || !m_waiting || m_steps[m_next].reply != 0x20) return;
            if (((flags & 4) != 0) != m_state.enabled) { m_result=-3; return; }
            Advance();
        }
        void Fail() { if (Pending()) m_result=-3; }
        std::optional<std::int32_t> Result() const { return m_result; }
        std::optional<std::int32_t> TakeCompletion()
        {
            if (!m_result || m_published) return {};
            m_published=true; return m_result;
        }
        const auto& Steps() const { return m_steps; }
        State FinalState() const { return m_state; }
        unsigned Command() const { return m_command; }
        std::uint32_t generation{};
    private:
        void Add(std::vector<std::uint8_t> packet,std::uint8_t reply=0x22) { m_steps.push_back({std::move(packet),reply}); }
        void Write(std::uint8_t address,std::span<const std::uint8_t> bytes)
        {
            std::vector<std::uint8_t> packet(22);
            packet[0]=0x16;packet[1]=4;packet[2]=0xa2;packet[3]=0;packet[4]=address;packet[5]=static_cast<std::uint8_t>(bytes.size());
            std::copy(bytes.begin(),bytes.end(),packet.begin()+6); Add(std::move(packet));
        }
        void Write(std::uint8_t address,std::initializer_list<std::uint8_t> bytes) { Write(address,std::span<const std::uint8_t>(bytes.begin(),bytes.size())); }
        void Advance() { m_waiting=false; if (++m_next==m_steps.size()) m_result=0; }
        std::vector<Step> m_steps;
        unsigned m_next{},m_command{};
        bool m_waiting{},m_published{true};
        std::optional<std::int32_t> m_result{0};
        Clock::time_point m_deadline{};
        State m_state;
    };

    class StatusRefresh
    {
    public:
        void Sent(std::uint8_t) {}
        bool Received(std::uint8_t,std::uint8_t,bool) const
        {
            // No request ID distinguishes a battery reply from overlapping hotplug.
            // Restore just the data-report command even when flags/mode match.
            return true;
        }
    };
}
