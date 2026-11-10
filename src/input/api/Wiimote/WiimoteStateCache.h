#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

template<class State, std::uint32_t DefaultDelay>
class WiimoteStateCache
{
public:
    using Snapshot = std::shared_ptr<const State>;
    struct Entry
    {
        Snapshot sample;
        bool speakerRequested=false, speakerMuted=true, speakerPlaying=false, speakerPending=false;
        std::uint32_t packetDelay=DefaultDelay;
    };
    Entry Read(std::size_t index) const
    {
        std::shared_lock lock(m_mutex);
        const auto found=m_entries.find(index);
        return found==m_entries.end() ? Entry{} : found->second;
    }
    void Publish(std::size_t index, const State& state)
    {
        auto sample=std::make_shared<const State>(state);
        Snapshot retired;
        {
            std::unique_lock lock(m_mutex);
            retired=std::exchange(m_entries[index].sample,std::move(sample));
        }
        // Destroy old motion histories outside the short publication lock.
    }
    void Clear(std::size_t index)
    {
        Snapshot retired;
        {
            std::unique_lock lock(m_mutex);
            auto& entry=m_entries[index];
            retired=std::move(entry.sample);
            entry.speakerRequested=false;entry.speakerMuted=true;entry.speakerPlaying=false;entry.speakerPending=false;
        }
    }
    void SetSpeaker(std::size_t index, bool requested, bool muted, bool playing=true, bool pending=false)
    {
        std::unique_lock lock(m_mutex);
        auto& entry=m_entries[index];entry.speakerRequested=requested;entry.speakerMuted=muted;entry.speakerPlaying=playing&&requested;entry.speakerPending=pending;
    }
    void SetDelay(std::size_t index, std::uint32_t delay)
    {
        if(Read(index).packetDelay==delay) return;
        std::unique_lock lock(m_mutex);
        m_entries[index].packetDelay=delay;
    }
private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::size_t,Entry> m_entries;
};
