#pragma once
#include "Cafe/OS/libs/padscore/YamahaAdpcm.h"
#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

namespace WiimoteSpeaker
{
    // Physical-output conversion only: the guest retains its normal 6 kHz stream.
    class CompatibilityStream
    {
    public:
        void Reset() { *this = CompatibilityStream{}; }
        std::vector<std::uint8_t> Convert(std::span<const std::uint8_t> input)
        {
            std::vector<std::uint8_t> output;
            output.reserve((input.size()+1)/2);
            for (const auto byte : input)
                for (auto nibble : {unsigned(byte >> 4), unsigned(byte & 15)})
                {
                    constexpr std::array<int,8> scale{230,230,230,230,307,409,512,614};
                    m_decoder.predictor = m_decoder.Decode(nibble);
                    m_decoder.step = std::clamp(m_decoder.step*scale[nibble&7]/256,127,24576);
                    m_history[m_position] = m_decoder.predictor;
                    if ((m_samples++ & 1) == 0)
                    {
                        double filtered = 0;
                        const auto& taps = Taps();
                        for (std::size_t i=0;i<taps.size();++i)
                            filtered += taps[i]*m_history[(m_position+taps.size()-i)%taps.size()];
                        const auto sample = static_cast<std::int16_t>(std::clamp(std::lround(filtered),-32768l,32767l));
                        const auto encoded = m_encoder.Encode(sample);
                        if (m_high) output.push_back(static_cast<std::uint8_t>((m_nibble<<4)|encoded));
                        else m_nibble = encoded;
                        m_high = !m_high;
                    }
                    m_position = (m_position+1)%m_history.size();
                }
            return output;
        }
    private:
        static const std::array<double,63>& Taps()
        {
            static const auto taps = [] {
                std::array<double,63> result{};
                double total=0;
                for (std::size_t i=0;i<result.size();++i)
                {
                    const double position=static_cast<double>(i)-31;
                    const double sinc=position==0 ? 0.45 : std::sin(std::numbers::pi*0.45*position)/(std::numbers::pi*position);
                    result[i]=sinc*(0.5-0.5*std::cos(2*std::numbers::pi*i/62));
                    total+=result[i];
                }
                for (auto& value : result) value/=total;
                return result;
            }();
            return taps;
        }
        padscore::YamahaAdpcm m_decoder{},m_encoder{};
        std::array<double,63> m_history{};
        std::size_t m_position=0;
        std::uint64_t m_samples=0;
        std::uint8_t m_nibble=0;
        bool m_high=false;
    };

    inline bool AppendPacket(std::vector<std::uint8_t>& packet,std::span<const std::uint8_t> bytes)
    {
        if(packet.size()!=22 || packet[0]!=0x18 || (packet[1]>>3)+bytes.size()>20)return false;
        const unsigned length=packet[1]>>3;
        std::copy(bytes.begin(),bytes.end(),packet.begin()+2+length);
        packet[1]=static_cast<std::uint8_t>(((length+bytes.size())<<3)|(packet[1]&7));
        return true;
    }
}
