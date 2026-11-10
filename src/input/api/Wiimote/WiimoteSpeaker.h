#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace WiimoteSpeaker
{
    constexpr unsigned SampleRate = 6000;
    constexpr std::uint8_t VolumeFromPercent(unsigned percent)
    {
        return static_cast<std::uint8_t>((std::min(100u, percent) * 127 + 50) / 100);
    }
    constexpr unsigned VolumePercent(std::uint8_t volume)
    {
        return (std::min<unsigned>(127, volume) * 100 + 63) / 127;
    }
    inline std::vector<std::uint8_t> Volume(std::uint8_t volume)
    {
        std::vector<std::uint8_t> packet(22);
        packet[0] = 0x16; packet[1] = 4; packet[2] = 0xa2;
        packet[4] = 5; packet[5] = 1; packet[6] = std::min<std::uint8_t>(127, volume);
        return packet;
    }
    inline bool IsVolume(std::span<const std::uint8_t> packet)
    {
        return packet.size() == 22 && packet[0] == 0x16 && (packet[1] & ~1u) == 4 &&
            packet[2] == 0xa2 && packet[3] == 0 && packet[4] == 5 && packet[5] == 1;
    }
    template<class Queue>
    void QueueVolume(Queue& queue, std::size_t index, std::uint8_t volume)
    {
        queue.remove_if([&](const auto& item) {
            return item.first == index && !item.controlGeneration && IsVolume(item.second);
        });
        // Coalesce slider events and use the writer's existing memory-ACK barrier.
        const auto audio = std::find_if(queue.begin(), queue.end(), [&](const auto& item) {
            return item.first == index && item.second[0] == 0x18;
        });
        queue.emplace(audio, index, Volume(volume));
    }
    inline std::vector<std::uint8_t> Data(std::span<const std::uint8_t> audio)
    {
        if (audio.empty() || audio.size() > 20) return {};
        std::vector<std::uint8_t> packet(22);
        packet[0] = 0x18; packet[1] = static_cast<std::uint8_t>(audio.size() << 3);
        std::copy(audio.begin(), audio.end(), packet.begin() + 2);
        return packet;
    }
    constexpr std::array<std::uint8_t, 7> Configuration(std::uint8_t volume,bool compatibility=false)
    {
        // Real-remotes' ADPCM sample clock is twice the nominal 6 MHz divider.
        return {0, 0, static_cast<std::uint8_t>(compatibility?0xa0:0xd0), static_cast<std::uint8_t>(compatibility?0x0f:0x07), std::min<std::uint8_t>(127, volume), 0x0c, 0x0e};
    }
}
