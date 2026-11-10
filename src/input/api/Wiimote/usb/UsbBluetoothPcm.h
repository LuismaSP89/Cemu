#pragma once
#include "UsbBluetoothProtocol.h"
#include "Cafe/OS/libs/padscore/YamahaAdpcm.h"
#include <cmath>
#include <memory>
#include <numeric>
#include <numbers>

namespace UsbBluetooth {
class PcmRateConverter {
public:
    void Configure(unsigned source,unsigned target) {
        m_source=source;m_target=target;
        m_filter.reset();
        if(source!=target) {
            auto filter=std::make_shared<Filter>();
            filter->phaseCount=std::min(64u,target/std::gcd(source,target));
            filter->taps.resize(filter->phaseCount+1);
            const double cutoff=0.45*target/source;
            for(unsigned phase=0;phase<=filter->phaseCount;++phase) {
                auto& taps=filter->taps[phase];double total=0;
                const double delay=static_cast<double>(phase)/filter->phaseCount;
                for(std::size_t i=0;i<taps.size();++i) {
                    const double x=static_cast<double>(i)-31-delay;
                    const double sinc=x==0?2*cutoff:std::sin(2*std::numbers::pi*cutoff*x)/(std::numbers::pi*x);
                    taps[i]=sinc*(0.5-0.5*std::cos(2*std::numbers::pi*i/62));total+=taps[i];
                }
                for(auto& value:taps)value/=total;
            }
            m_filter=std::move(filter);
        }
        Reset();
    }
    void Reset(){m_history={};m_position=m_phase=0;}
    void SetGain(unsigned gain){m_gain=gain==2?2:1;}
    static double ApplyGain(double sample,unsigned gain) {
        if(gain!=2)return sample;
        const double scaled=sample*2,magnitude=std::abs(scaled);
        if(magnitude<=24576)return scaled;
        // Keep quiet cues linear; softly limit louder sources instead of clipping.
        const double excess=magnitude-24576;
        return std::copysign(24576+7936*excess/(excess+7936),scaled);
    }
    std::optional<std::uint8_t> Push(std::int16_t sample) {
        m_history[m_position]=sample;
        m_phase+=m_target;
        std::optional<std::uint8_t> result;
        if(m_phase>=m_source) {
            m_phase-=m_source;
            double filtered=sample;
            if(m_target!=m_source) {
                // Remainder is the fractional delay from this input sample to
                // the uniform output grid, not a reason to snap to its time.
                const auto phase=static_cast<unsigned>((std::uint64_t(m_phase)*m_filter->phaseCount+m_target/2)/m_target);
                const auto& taps=m_filter->taps[phase];
                filtered=0;
                for(std::size_t i=0;i<taps.size();++i)filtered+=taps[i]*m_history[(m_position+taps.size()-i)%taps.size()];
            }
            result=static_cast<std::uint8_t>(std::clamp(std::lround(ApplyGain(filtered,m_gain)/256),-128l,127l));
        }
        m_position=(m_position+1)%m_history.size();return result;
    }
private:
    struct Filter {unsigned phaseCount{};std::vector<std::array<double,63>> taps;};
    std::shared_ptr<const Filter> m_filter;
    std::array<std::int16_t,63> m_history{};
    std::size_t m_position{};
    unsigned m_source=6000,m_target=2000,m_phase{},m_gain=1;
};

// USB-only output compatibility: resample before reducing the physical clock.
class PcmSpeaker {
public:
    std::vector<ScheduledReport> Translate(std::span<const std::uint8_t> report,std::uint64_t nowUs) {
        m_accepted=true;
        if(report.empty()) {m_accepted=false;return {};}
        Bytes copy(report.begin(),report.end());
        if(report.size()==22 && report[0]==0x16 && (report[1]&4) && report[2]==0xa2 && report[3]==0) {
            const auto address=report[4],length=report[5];
            if(address==1 && length==7 && report[7]!=0) {m_pcm=false;ResetDecoder();}
            else if(address==1 && length==7 && report[7]==0) {
                const auto divider=Read16(report,8);
                if(!divider){m_pcm=false;ResetDecoder();return {{std::move(copy),0}};}
                m_pcm=true;const auto sourceRate=12000000u/divider;m_rate=std::min(m_rateLimit,sourceRate);
                m_converter.Configure(sourceRate,m_rate);ResetDecoder();
                copy[7]=0x40;copy[10]=Volume(report[10]);
                const auto wireDivider=static_cast<std::uint16_t>(sourceRate>m_rateLimit?12000000u/m_rateLimit:divider);
                copy[8]=static_cast<std::uint8_t>(wireDivider);copy[9]=static_cast<std::uint8_t>(wireDivider>>8);
            } else if(address==5 && length==1 && m_pcm) copy[6]=Volume(report[6]);
            else if((address==1 || address==8) && length==1) ResetDecoder();
        }
        if(report[0]==0x14 && report.size()>=2 && !(report[1]&4)){m_pcm=false;ResetDecoder();}
        if(report[0]!=0x18 || !m_pcm) return {{std::move(copy),0}};
        if(report.size()<2) {m_accepted=false;return {};}
        const auto length=unsigned(report[1]>>3);
        if(!length || length>20 || report.size()<length+2 || !m_rate) {m_accepted=false;return {};}
        const auto next=m_anchorUs+m_frames*1000000/m_rate;
        if(m_started && m_buffer.empty() && m_pending.empty() && nowUs>next+2000) {m_started=false;m_converter.Reset();}
        if(m_buffer.empty() && m_pending.empty())m_bufferSince=nowUs;
        m_lastInputUs=nowUs;m_pendingFlags=report[1]&7;
        constexpr std::array<int,8> factors{230,230,230,230,307,409,512,614};
        for(unsigned i=0;i<length;++i) for(auto nibble:{unsigned(report[i+2]>>4),unsigned(report[i+2]&15)}) {
            m_decoder.predictor=m_decoder.Decode(nibble);
            m_decoder.step=std::clamp(m_decoder.step*factors[nibble&7]/256,127,24576);
            if(const auto pcm=m_converter.Push(static_cast<std::int16_t>(m_decoder.predictor)))m_pending.push_back(*pcm);
        }
        while(m_pending.size()>=20)Pack(20);
        return Flush(nowUs);
    }
    std::vector<ScheduledReport> Flush(std::uint64_t nowUs) {
        if(!m_pending.empty() && nowUs>=m_lastInputUs+16000)Pack(m_pending.size());
        if(m_buffer.empty() || (!m_started && m_buffer.size()<2 && nowUs-m_bufferSince<16000)) return {};
        if(!m_started) {m_anchorUs=nowUs;m_frames=0;m_started=true;}
        std::vector<ScheduledReport> output;
        for(auto& packet:m_buffer) {
            const auto due=m_anchorUs+m_frames*1000000/m_rate;
            m_frames+=packet[1]>>3;
            output.push_back({std::move(packet),due});
        }
        m_buffer.clear();
        return output;
    }
    bool Buffered() const {return !m_buffer.empty() || !m_pending.empty();}
    bool Accepted() const {return m_accepted;}
    unsigned Rate() const {return m_rate;}
    void SetGain(unsigned gain){m_converter.SetGain(gain);}
    void SetRateLimit(unsigned rate){m_rateLimit=rate==2500 || rate==3000?rate:2000;}
private:
    static std::uint8_t Volume(std::uint8_t value){return static_cast<std::uint8_t>((std::min<unsigned>(127,value)*255+63)/127);}
    void Pack(std::size_t count){
        Bytes packet(22);packet[0]=0x18;packet[1]=static_cast<std::uint8_t>((count<<3)|m_pendingFlags);
        std::copy_n(m_pending.begin(),count,packet.begin()+2);m_pending.erase(m_pending.begin(),m_pending.begin()+count);
        m_buffer.push_back(std::move(packet));
    }
    void ResetDecoder(){m_decoder={};m_converter.Reset();m_anchorUs=m_frames=m_bufferSince=m_lastInputUs=0;m_buffer.clear();m_pending.clear();m_started=false;}
    padscore::YamahaAdpcm m_decoder;
    PcmRateConverter m_converter;
    bool m_pcm=false,m_accepted=true,m_started=false;
    unsigned m_rate=2000,m_rateLimit=2000;
    std::uint8_t m_pendingFlags{};
    std::uint64_t m_anchorUs{},m_frames{},m_bufferSince{},m_lastInputUs{};
    std::vector<Bytes> m_buffer;
    Bytes m_pending;
};
} // namespace UsbBluetooth
