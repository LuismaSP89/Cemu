#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>

namespace UsbBluetooth {
struct OutputTiming {
    std::uint64_t submitted{},completed{},failed{},maxTransferUs{},maxQueueDepth{},creditBlockedTicks{};
    std::uint64_t scheduledSubmissions{},maxLateUs{},lateOver2ms{};
    void ObserveQueue(std::size_t depth,bool blocked) {maxQueueDepth=std::max<std::uint64_t>(maxQueueDepth,depth);if(blocked)++creditBlockedTicks;}
    void Complete(std::uint64_t elapsed,bool success) {++completed;if(!success)++failed;maxTransferUs=std::max(maxTransferUs,elapsed);}
    void Deadline(std::uint64_t now,std::uint64_t due) {
        if(!due)return;
        ++scheduledSubmissions;const auto late=now>due?now-due:0;
        maxLateUs=std::max(maxLateUs,late);if(late>2000)++lateOver2ms;
    }
    std::string Summary(unsigned queued,unsigned rejected,unsigned inflight,unsigned limit) const {
        return "USB speaker timing: accepted="+std::to_string(queued)+" rejected="+std::to_string(rejected)+
            " submitted="+std::to_string(submitted)+" usb_completed="+std::to_string(completed)+" usb_failed="+std::to_string(failed)+
            " max_usb_us="+std::to_string(maxTransferUs)+" peak_acl_queue="+std::to_string(maxQueueDepth)+
            " credit_blocked_ticks="+std::to_string(creditBlockedTicks)+" inflight="+std::to_string(inflight)+" limit="+std::to_string(limit)+
            " scheduled="+std::to_string(scheduledSubmissions)+" max_late_us="+std::to_string(maxLateUs)+" late_over_2ms="+std::to_string(lateOver2ms);
    }
};
// Opt-in, bounded metadata only: never record addresses, keys or HID payloads.
class SessionTrace {
public:
    static constexpr std::size_t Limit = 64 * 1024;
    explicit SessionTrace(const std::filesystem::path& path = {}) {
        if (!path.empty() && path.is_absolute()) m_file.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
        Line("USB session metadata; no addresses, PINs, keys, audio or button payloads");
    }
    bool Enabled() const { return m_file.is_open(); }
    void Line(const std::string& text) {
        if (!m_file || m_bytes >= Limit) return;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count();
        const auto line = std::to_string(ms) + " " + text + "\n";
        if (line.size() > Limit - m_bytes) { m_bytes = Limit; return; }
        m_file << line; m_file.flush(); m_bytes += line.size();
    }
    void Command(std::span<const std::uint8_t> b) {
        if (!Enabled() || b.size() < 3 || b[2] != b.size() - 3) return;
        std::ostringstream out; out << "HCI TX opcode=0x" << std::hex << std::setfill('0') << std::setw(4) << Word(b, 0);
        Line(out.str());
    }
    void Event(std::span<const std::uint8_t> b) {
        if (!Enabled() || b.size() < 2 || b[1] != b.size() - 2 || b[0] == 0x13) return;
        std::ostringstream out; out << "HCI RX event=0x" << std::hex << std::setfill('0') << std::setw(2) << unsigned(b[0]);
        if (b[0] == 0x05 && b.size() == 6) {
            out << " status=0x" << std::setw(2) << unsigned(b[2]) << " handle=0x" << std::setw(4) << Word(b, 3)
                << " reason=0x" << std::setw(2) << unsigned(b[5]);
        } else if (b[0] == 0x03 && b.size() == 13) {
            out << " status=0x" << std::setw(2) << unsigned(b[2]) << " handle=0x" << std::setw(4) << Word(b, 3);
        } else if (b[0] == 0x0e && b.size() >= 6) {
            out << " opcode=0x" << std::setw(4) << Word(b, 3) << " status=0x" << std::setw(2) << unsigned(b[5]);
        } else if (b[0] == 0x0f && b.size() == 6) {
            out << " opcode=0x" << std::setw(4) << Word(b, 4) << " status=0x" << std::setw(2) << unsigned(b[2]);
        }
        Line(out.str());
    }
    void HidControl(std::uint16_t handle, std::uint8_t transaction) {
        if (!Enabled()) return;
        std::ostringstream out; out << "HID control RX handle=0x" << std::hex << std::setfill('0') << std::setw(4) << handle
            << " transaction=0x" << std::setw(2) << unsigned(transaction);
        Line(out.str());
    }
private:
    static unsigned Word(std::span<const std::uint8_t> b, std::size_t off) { return unsigned(b[off]) | (unsigned(b[off + 1]) << 8); }
    std::ofstream m_file;
    std::size_t m_bytes{};
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};
} // namespace UsbBluetooth
