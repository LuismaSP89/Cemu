#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

// Writer retires every request before its stable buffer/event can be destroyed.
template<class Writer>
class WiimoteAudioWriteQueue
{
public:
    explicit WiimoteAudioWriteQueue(Writer& writer):m_writer(writer){}
    ~WiimoteAudioWriteQueue(){Drain();}
    WiimoteAudioWriteQueue(const WiimoteAudioWriteQueue&)=delete;
    WiimoteAudioWriteQueue& operator=(const WiimoteAudioWriteQueue&)=delete;
    bool Submit(std::span<const std::uint8_t> report)
    {
        if(m_failed || report.size()!=22 || report[0]!=0x18 || (report[1]>>3)==0 || (report[1]>>3)>20)return false;
        auto& slot=m_slots[m_next%m_slots.size()];
        if(slot.pending && !Retire(slot))return false;
        std::copy(report.begin(),report.end(),slot.bytes.begin());
        if(!m_writer.Begin(slot.token,slot.bytes)){m_failed=true;return false;}
        slot.pending=true;++m_next;return true;
    }
    bool Drain() noexcept
    {
        for(std::size_t i=0;i<m_slots.size();++i)
        {
            auto& slot=m_slots[(m_next+i)%m_slots.size()];
            if(slot.pending)Retire(slot);
        }
        return !m_failed;
    }
private:
    struct Slot{std::array<std::uint8_t,22> bytes{};typename Writer::Token token;bool pending{};};
    bool Retire(Slot& slot) noexcept
    {
        const bool complete=m_writer.Finish(slot.token,slot.bytes.size());
        slot.pending=false;m_failed|=!complete;return complete;
    }
    Writer& m_writer;
    std::array<Slot,2> m_slots;
    std::size_t m_next{};
    bool m_failed{};
};
