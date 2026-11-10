#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace UsbBluetooth {
using Bytes = std::vector<std::uint8_t>;
using Address = std::array<std::uint8_t, 6>;
inline std::uint16_t Read16(std::span<const std::uint8_t> b, size_t p) {
    return std::uint16_t(b[p] | (std::uint16_t(b[p + 1]) << 8));
}
inline void Put16(Bytes& b, std::uint16_t n) { b.push_back(n & 255); b.push_back(n >> 8); }
inline Address ReadAddress(std::span<const std::uint8_t> b, size_t p) {
    Address a{}; std::copy_n(b.begin() + p, 6, a.begin()); return a;
}
inline bool IsWiiName(std::string_view name) {
    return name == "Nintendo RVL-CNT-01" || name == "Nintendo RVL-CNT-01-TR";
}
inline bool IsDedicatedAdapter(std::uint16_t vid, std::uint16_t pid) {
    return vid == 0x0b05 && pid == 0x1d70;
}

struct Channel {
    std::uint16_t local{}, peer{};
    std::uint8_t connectId{}, configId{};
    bool inboundConfigured{}, outboundConfigured{};
    bool Ready() const { return peer && inboundConfigured && outboundConfigured; }
};
struct Remote {
    Address address{};
    std::string name;
    std::uint16_t handle = 0xffff;
    Channel control, interrupt;
    bool connecting = false;
    bool authenticated = false, authenticating = false, bonded = false, incoming = false;
    std::uint64_t progress{};
    Bytes assembly;
    std::deque<Bytes> reports;
    unsigned outstanding{};
    unsigned classOfDevice{};
    bool Ready() const { return handle != 0xffff && authenticated && control.Ready() && interrupt.Ready(); }
};
struct Bond { Address address{}; std::string name; std::array<std::uint8_t,16> key{}; };
struct ScheduledReport { Bytes data; std::uint64_t dueUs{}; };
struct AclPacket { Bytes data; std::uint64_t dueUs{}; };

// Transport-independent BR/EDR HID state machine. All methods run under the host's mutex.
class Session {
public:
    static constexpr size_t MaxRemotes = 4;
    void Start(std::uint64_t now) {
        m_now = now; m_ready = false; m_error.clear(); m_commands.clear(); m_acl.clear();
        m_pending = 0; m_credits = 1; m_inquiry = false; m_nextInquiry = now;
        m_inquiryCancel = false; m_closing = false; m_audioUntil = m_syncUntil = 0;
        m_remotes.clear(); m_keys.clear(); m_bondRevision = 0;
        m_rejectedNames.clear();
        m_audioQueued=m_audioRejected=0;m_lastAclDueUs=0;
        m_inquiryResults=m_nameAccepted=m_nameRejected=m_capacityRejected=0;
        m_lastSeenClass=m_lastNamedClass=m_lastNameModel=0;
        m_aclLimit = 0; m_aclMtu = 0; m_inflight = 0;
        Command(0x0c03, {}); m_status = "Initializing dedicated USB Bluetooth";
    }
    void Tick(std::uint64_t now) {
        m_now = now;
        if (m_pending && now >= m_deadline) Fail("Bluetooth HCI command timed out; no firmware was uploaded");
        if (m_closing || !m_error.empty()) return;
        std::erase_if(m_rejectedNames,[&](const auto& entry){return entry.second<=now;});
        for (auto& r : m_remotes) {
            if (r.handle == 0xffff && r.connecting && now - r.progress > 20000) r.connecting = false;
            if (r.handle != 0xffff && !r.Ready() && now - r.progress > 15000) Disconnect(r);
        }
        const auto connected = std::count_if(m_remotes.begin(), m_remotes.end(), [](const auto& r) { return r.Ready(); });
        const bool connecting=std::any_of(m_remotes.begin(),m_remotes.end(),[](const auto& r){return r.connecting || (r.handle!=0xffff && !r.Ready());});
        // New red-SYNC remotes must be discoverable even with saved players.
        // Brief inquiries yield to incoming reconnection and active speaker cues.
        const bool allowInquiry = connected < MaxRemotes && (now < m_syncUntil || now >= m_audioUntil);
        if (!allowInquiry) CancelInquiry();
        if (m_ready && allowInquiry && !connecting && !m_inquiry && now >= m_nextInquiry && connected < MaxRemotes && !m_pending && m_commands.empty()) {
            Command(0x0401, {0x33, 0x8b, 0x9e, 1, 0});
            m_inquiry = true; m_nextInquiry = now + 5000;
        }
    }
    void RequestSync() { m_nextInquiry = m_now; m_syncUntil = m_now + 20000; }
    bool Ready() const { return m_ready && !m_closing && m_error.empty(); }
    void BeginShutdown() {
        if (m_closing) return;
        m_closing = true; m_commands.clear(); m_acl.clear(); m_inquiryCancel=false;CancelInquiry();
        for (const auto& r : m_remotes) if (r.handle != 0xffff) {
            Bytes b; Put16(b,r.handle); b.push_back(0x13); Command(0x0406,b);
        }
        m_status = "Disconnecting USB Wiimotes before releasing adapter";
    }
    bool ShutdownPending() const {
        return m_pending || !m_commands.empty() || std::any_of(m_remotes.begin(),m_remotes.end(),[](const auto& r){return r.handle != 0xffff;});
    }
    const Address& LocalAddress() const { return m_local; }
    unsigned BondRevision() const { return m_bondRevision; }
    std::vector<Bond> Bonds() const {
        std::vector<Bond> result;
        for (const auto& r : m_remotes) if (r.bonded) {
            const auto key=m_keys.find(r.address);
            if(key != m_keys.end()) result.push_back({r.address,r.name,key->second});
        }
        return result;
    }
    bool RestoreBonds(const std::vector<Bond>& bonds) {
        if(bonds.size()>MaxRemotes || std::any_of(m_remotes.begin(),m_remotes.end(),[](const auto& r){return r.handle!=0xffff;})) return false;
        std::vector<Address> seen;
        for(const auto& b:bonds) {
            if(!IsWiiName(b.name) || b.address==m_local || b.address==Address{} ||
                std::find(seen.begin(),seen.end(),b.address)!=seen.end()) return false;
            seen.push_back(b.address);
        }
        m_remotes.clear(); m_keys.clear();
        for(const auto& b:bonds) { Remote r; r.address=b.address;r.name=b.name;r.bonded=true;m_remotes.push_back(r);m_keys[b.address]=b.key; }
        return true;
    }
    const std::string& Status() const { return m_status; }
    const std::string& Error() const { return m_error; }
    const std::vector<Remote>& Remotes() const { return m_remotes; }
    std::uint16_t Manufacturer() const { return m_manufacturer; }
    std::uint16_t Subversion() const { return m_subversion; }
    std::uint8_t HciVersion() const { return m_hciVersion; }
    unsigned MalformedPackets() const { return m_malformed; }
    std::size_t QueuedAcl() const { return m_acl.size(); }
    unsigned InflightAcl() const { return m_inflight; }
    unsigned AclLimit() const { return m_aclLimit; }
    unsigned AudioQueued() const { return m_audioQueued; }
    unsigned AudioRejected() const { return m_audioRejected; }
    std::string DiscoverySummary() const {
        return "USB discovery: results="+std::to_string(m_inquiryResults)+" last_seen_class="+std::to_string(m_lastSeenClass)+
            " names_accepted="+std::to_string(m_nameAccepted)+" names_rejected="+std::to_string(m_nameRejected)+
            " last_named_class="+std::to_string(m_lastNamedClass)+" last_name_model="+std::to_string(m_lastNameModel)+
            " capacity_rejected="+std::to_string(m_capacityRejected);
    }
    bool AudioActive() const {return Ready() && m_now<m_audioUntil;}
    std::uint64_t LastAclDueUs() const {return m_lastAclDueUs;}
    void TransportFailed() { Fail("Dedicated Bluetooth USB transport failed or disconnected"); }

    std::optional<Bytes> TakeCommand() {
        if (!m_error.empty() || m_pending || !m_credits || m_commands.empty()) return {};
        auto b = std::move(m_commands.front()); m_commands.pop_front();
        m_pending = Read16(b, 0); --m_credits; m_deadline = m_now + 2000;
        return b;
    }
    std::optional<Bytes> TakeAcl(std::uint64_t nowUs=0) {
        if (!Ready() || m_acl.empty() || m_inflight >= m_aclLimit) return {};
        if(!nowUs) nowUs=m_now*1000;
        std::vector<std::uint16_t> blocked;
        for(auto it=m_acl.begin();it!=m_acl.end();) {
            const auto handle=Read16(it->data,0)&0xfff;
            if(std::find(blocked.begin(),blocked.end(),handle)!=blocked.end()){++it;continue;}
            if(it->dueUs>nowUs){blocked.push_back(handle);++it;continue;}
            const auto dueUs=it->dueUs;auto b=std::move(it->data);it=m_acl.erase(it);
            if(auto* r=ByHandle(handle)){m_lastAclDueUs=dueUs;++r->outstanding;++m_inflight;return b;}
        }
        return {};
    }
    bool WriteReports(const Address& a,const std::vector<ScheduledReport>& reports) {
        auto* r=ByAddress(a);
        if(!Ready() || !r || !r->Ready() || reports.empty() || reports.size()>8 || m_acl.size()+reports.size()>384) return false;
        for(const auto& p:reports) if(p.data.empty() || p.data.size()>22) return false;
        for(const auto& p:reports) if(!WriteReport(a,p.data,p.dueUs)) return false;
        return true;
    }
    bool WriteReport(const Address& a, std::span<const std::uint8_t> report,std::uint64_t dueUs=0) {
        auto* r = ByAddress(a);
        const bool audio=!report.empty() && report[0]==0x18;
        if (!Ready() || !r || !r->Ready() || report.empty() || report.size() > 22 || m_acl.size() > 384) {if(audio)++m_audioRejected;return false;}
        if(report[0]==0x18) { m_audioUntil=m_now+500; if(m_now>=m_syncUntil) CancelInquiry(); }
        Bytes b{0xa2}; b.insert(b.end(), report.begin(), report.end());
        const bool accepted=Acl(*r, r->interrupt.peer, b,dueUs);
        if(audio) {if(accepted)++m_audioQueued;else ++m_audioRejected;}
        return accepted;
    }
    std::optional<Bytes> ReadReport(const Address& a) {
        auto* r = ByAddress(a);
        if (!Ready() || !r || !r->Ready()) return {};
        if (r->reports.empty()) return Bytes{};
        auto b = std::move(r->reports.front()); r->reports.pop_front(); return b;
    }

    void Event(std::span<const std::uint8_t> b) {
        if (b.size() < 2 || b[1] != b.size() - 2) { ++m_malformed; return; }
        const auto event = b[0]; const auto p = b.subspan(2);
        if (event == 0x0e) {
            if (p.size() < 4) { ++m_malformed; return; }
            m_credits = p[0]; const auto op = Read16(p, 1);
            if (op != m_pending) return;
            m_pending = 0;
            if (p[3]) {
                if (op == 0x0402) { m_inquiry = false; m_inquiryCancel=false; return; }
                Fail("Bluetooth controller rejected an HCI command; firmware compatibility is unverified"); return;
            }
            Initialized(op, p.subspan(3)); return;
        }
        if (event == 0x0f) {
            if (p.size() != 4) { ++m_malformed; return; }
            m_credits = p[1]; const auto op = Read16(p, 2);
            if (op != m_pending) return;
            m_pending = 0;
            if (p[0]) {
                if (op == 0x0401) { m_inquiry = false; m_nextInquiry = m_now + 3000; }
                else if (op == 0x0405 || op == 0x0419) { for (auto& r : m_remotes) if (r.handle == 0xffff) r.connecting = false; }
                else Fail("Bluetooth controller rejected connection setup");
            }
            return;
        }
        if (event == 0x01) { m_inquiry = false; m_inquiryCancel=false; m_nextInquiry = std::max(m_nextInquiry,m_now + 1000); return; }
        if (event == 0x02 || event == 0x22) { Inquiry(p, event == 0x22); return; }
        if (event == 0x07) { Name(p); return; }
        if (event == 0x03) { Connected(p); return; }
        if (event == 0x04) {
            if (p.size() < 10) { ++m_malformed; return; }
            const auto a = ReadAddress(p, 0); auto* r = ByAddress(a);
            Bytes data(a.begin(), a.end());
            if (!m_closing && r && IsWiiName(r->name) && p[9] == 1) {
                r->incoming=true;r->connecting=true;r->progress=m_now;CancelInquiry();m_nextInquiry=std::max(m_nextInquiry,m_now+5000);
                data.push_back(0);Command(0x0409,data);
            }
            else { data.push_back(0x0f); Command(0x040a, data); }
            return;
        }
        if (event == 0x05) {
            if (p.size() != 4) { ++m_malformed; return; }
            if (auto* r = ByHandle(Read16(p, 1))) {
                if (p[0]) { m_status = "Bluetooth disconnect failed; Wii Remote remains connected"; return; }
                Purge(*r); ClearLink(*r);
                if (p[3] == 0x15) m_status = "Wii Remote powered off; adapter ready for reconnection";
                else if (p[3] == 0x14) m_status = "Wii Remote shut down for low resources; adapter remains ready";
                else m_status = "Wii Remote disconnected; adapter ready for reconnection";
            }
            return;
        }
        if (event == 0x13) {
            if (p.empty() || p.size() != size_t(1 + 4 * p[0])) { ++m_malformed; return; }
            for (unsigned i = 0; i < p[0]; ++i) {
                if (auto* r = ByHandle(Read16(p, 1 + i * 4))) {
                    const auto n = std::min<unsigned>(r->outstanding, Read16(p, 3 + i * 4));
                    r->outstanding -= n; m_inflight -= std::min(m_inflight, n);
                }
            }
            return;
        }
        if (event == 0x16 || event == 0x17) {
            if (p.size() != 6 || !ByAddress(ReadAddress(p, 0))) { ++m_malformed; return; }
            const auto a = ReadAddress(p, 0); Bytes data(a.begin(), a.end());
            if (event == 0x16) {
                data.push_back(6); data.insert(data.end(), m_local.begin(), m_local.end()); data.resize(23, 0);
                Command(0x040d, data);
            } else if (const auto it = m_keys.find(a); it != m_keys.end()) {
                data.insert(data.end(), it->second.begin(), it->second.end()); Command(0x040b, data);
            } else Command(0x040c, data);
            return;
        }
        if (event == 0x18) {
            if (p.size() != 23) { ++m_malformed; return; }
            const auto a = ReadAddress(p, 0);
            if (ByAddress(a) && p[22]==0) { std::array<std::uint8_t, 16> key{}; std::copy_n(p.begin() + 6, 16, key.begin()); m_keys[a] = key; ++m_bondRevision; }
            return;
        }
        if (event == 0x06) {
            if (p.size() != 3) { ++m_malformed; return; }
            if (auto* r = ByHandle(Read16(p, 1))) {
                r->authenticating=false;
                if (p[0]) Disconnect(*r);
                else {
                    r->authenticated=true;r->bonded=true;++m_bondRevision;
                    if (!r->incoming && !r->control.peer) OpenChannel(*r, r->control, 0x11);
                    else if (!r->incoming && r->control.Ready() && !r->interrupt.peer) OpenChannel(*r, r->interrupt, 0x13);
                }
            }
        }
    }

    void AclInput(std::span<const std::uint8_t> b) {
        if (b.size() < 4 || Read16(b, 2) != b.size() - 4) { ++m_malformed; return; }
        auto* r = ByHandle(Read16(b, 0) & 0xfff); if (!r) return;
        const auto flags = (Read16(b, 0) >> 12) & 3;
        if (flags == 0 || flags == 2) r->assembly.clear();
        else if (flags != 1 || r->assembly.empty()) { ++m_malformed; return; }
        if (r->assembly.size() + b.size() - 4 > 2048) { r->assembly.clear(); ++m_malformed; return; }
        r->assembly.insert(r->assembly.end(), b.begin() + 4, b.end());
        if (r->assembly.size() < 4) return;
        const auto expected = size_t(Read16(r->assembly, 0)) + 4;
        if (expected > 2048 || r->assembly.size() > expected) { r->assembly.clear(); ++m_malformed; return; }
        if (r->assembly.size() < expected) return;
        auto packet = std::move(r->assembly); r->assembly.clear();
        const auto cid = Read16(packet, 2); const auto payload = std::span(packet).subspan(4);
        r->progress = m_now;
        if (cid == 1) Signal(*r, payload);
        else if (r->Ready() && cid == r->interrupt.local && payload.size() >= 2 && payload.size() <= 23 && payload[0] == 0xa1) {
            if (r->reports.size() >= 128) r->reports.pop_front();
            r->reports.emplace_back(payload.begin() + 1, payload.end());
        }
    }

private:
    std::uint64_t m_now{}, m_deadline{}, m_nextInquiry{}, m_audioUntil{}, m_syncUntil{}, m_lastAclDueUs{};
    std::uint16_t m_pending{}, m_aclMtu{}, m_aclLimit{}, m_manufacturer{}, m_subversion{};
    std::uint8_t m_credits = 1, m_id = 0, m_hciVersion = 0;
    unsigned m_inflight{}, m_malformed{}, m_bondRevision{}, m_audioQueued{}, m_audioRejected{};
    unsigned m_inquiryResults{},m_nameAccepted{},m_nameRejected{},m_capacityRejected{};
    unsigned m_lastSeenClass{},m_lastNamedClass{},m_lastNameModel{};
    bool m_ready = false, m_inquiry = false, m_inquiryCancel=false, m_closing=false;
    Address m_local{};
    std::string m_status = "Disabled", m_error;
    std::vector<Remote> m_remotes;
    std::map<Address, std::array<std::uint8_t, 16>> m_keys;
    std::map<Address,std::uint64_t> m_rejectedNames;
    std::deque<Bytes> m_commands;
    std::deque<AclPacket> m_acl;

    void Fail(std::string error) { m_ready = false; m_error = std::move(error); m_status = m_error; m_commands.clear(); m_acl.clear(); }
    void Command(std::uint16_t op, std::span<const std::uint8_t> data) {
        if (m_commands.size() >= 64 || data.size() > 255) { Fail("Bluetooth command queue limit reached"); return; }
        Bytes b; Put16(b, op); b.push_back(static_cast<std::uint8_t>(data.size()));
        b.insert(b.end(), data.begin(), data.end()); m_commands.push_back(std::move(b));
    }
    void Command(std::uint16_t op, std::initializer_list<std::uint8_t> data) { Command(op, std::span(data)); }
    void CancelInquiry() {
        if(m_inquiry && !m_inquiryCancel) {Command(0x0402,{});m_inquiryCancel=true;}
    }
    void Authenticate(Remote& r) {
        if(r.authenticating) return;
        r.authenticating=true;Bytes b;Put16(b,r.handle);Command(0x0411,b);
    }
    Remote* ByAddress(const Address& a) {
        const auto it = std::find_if(m_remotes.begin(), m_remotes.end(), [&](const auto& r) { return r.address == a; });
        return it == m_remotes.end() ? nullptr : &*it;
    }
    Remote* ByHandle(std::uint16_t h) {
        if (h == 0xffff) return nullptr;
        const auto it = std::find_if(m_remotes.begin(), m_remotes.end(), [&](const auto& r) { return r.handle == h; });
        return it == m_remotes.end() ? nullptr : &*it;
    }
    static void ClearLink(Remote& r) {
        r.handle = 0xffff; r.connecting = false; r.control = {}; r.interrupt = {};
        r.authenticated=false;r.authenticating=false;
        r.incoming=false;
        r.assembly.clear(); r.reports.clear(); r.outstanding = 0;
    }
    void Purge(Remote& r) {
        m_inflight -= std::min(m_inflight, r.outstanding);
        std::erase_if(m_acl, [&](const auto& b) { return (Read16(b.data, 0) & 0xfff) == r.handle; });
    }
    void Disconnect(Remote& r) {
        if (r.handle == 0xffff) return;
        Bytes b; Put16(b, r.handle); b.push_back(0x13); Command(0x0406, b);
        Purge(r); ClearLink(r);
    }
    void Initialized(std::uint16_t op, std::span<const std::uint8_t> p) {
        if (op == 0x0c03) Command(0x1001, {});
        else if (op == 0x1001) {
            if (p.size() != 9) { Fail("Invalid HCI version response"); return; }
            m_hciVersion = p[1]; m_manufacturer = Read16(p, 5); m_subversion = Read16(p, 7);
            Command(0x1005, {});
        } else if (op == 0x1005) {
            if (p.size() != 8 || Read16(p, 1) < 27 || !Read16(p, 4)) { Fail("Controller has no usable classic-Bluetooth buffers; firmware initialization may be required"); return; }
            m_aclMtu = std::min<std::uint16_t>(Read16(p, 1), 1021); m_aclLimit = std::min<std::uint16_t>(Read16(p, 4), 64);
            Command(0x1009, {});
        } else if (op == 0x1009) {
            if (p.size() != 7) { Fail("Invalid HCI address response"); return; }
            m_local = ReadAddress(p, 1);
            std::uint64_t mask = 0;
            for (unsigned e : {1,2,3,4,5,6,7,8,14,15,16,18,19,22,23,24,34,47}) mask |= std::uint64_t{1} << (e - 1);
            Bytes b; for (unsigned i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(mask >> (i * 8)));
            Command(0x0c01, b);
        } else if (op == 0x0c01) Command(0x0c20, {0});
        else if (op == 0x0c20) Command(0x0c45, {0});
        else if (op == 0x0c45) Command(0x0c1a, {3});
        else if (op == 0x0c1a) { m_ready = true; m_status = "Adapter ready; A reconnects saved remotes, red SYNC adds a remote to the next free controller slot"; m_nextInquiry = m_now+(m_keys.empty()?0:5000); }
        else if (op == 0x0402) {m_inquiry = false;m_inquiryCancel=false;}
    }
    void Inquiry(std::span<const std::uint8_t> p, bool rssi) {
        if (p.empty() || p.size() != size_t(1 + 14 * p[0])) { ++m_malformed; return; }
        for (unsigned i = 0; i < p[0]; ++i) {
            const auto b = p.subspan(1 + i * 14, 14); const auto cod = rssi ? 8u : 9u;
            ++m_inquiryResults;
            // Class bits vary (the hardware trace includes 0x0508). Identity
            // is validated by Name() before any connection/authentication.
            m_lastSeenClass=b[cod]|(unsigned(b[cod+1])<<8)|(unsigned(b[cod+2])<<16);
            const auto a = ReadAddress(b, 0); auto* r = ByAddress(a);
            if(const auto rejected=m_rejectedNames.find(a);rejected!=m_rejectedNames.end() && m_now<rejected->second)continue;
            if (!r) {
                const auto reusable=std::find_if(m_remotes.begin(),m_remotes.end(),[](const auto& candidate){return candidate.handle==0xffff && !candidate.connecting && !candidate.bonded;});
                if(reusable!=m_remotes.end()){m_keys.erase(reusable->address);*reusable=Remote{};r=&*reusable;}
                else {if(m_remotes.size()>=MaxRemotes){++m_capacityRejected;continue;}m_remotes.emplace_back();r=&m_remotes.back();}
                r->address = a;
            }
            r->classOfDevice=m_lastSeenClass;
            if (r->handle != 0xffff || r->connecting) continue;
            r->connecting = true; r->progress = m_now;
            CancelInquiry();
            Bytes data(a.begin(), a.end()); data.push_back(b[6]); data.push_back(0);
            Put16(data, Read16(b, rssi ? 11 : 12) | 0x8000); Command(0x0419, data);
        }
    }
    void Name(std::span<const std::uint8_t> p) {
        if (p.size() < 8) { ++m_malformed; return; }
        auto* r = ByAddress(ReadAddress(p, 1)); if (!r) return;
        // A reconnect may win while discovery is still resolving its name.
        if(r->incoming || r->handle!=0xffff)return;
        const auto end = std::find(p.begin() + 7, p.end(), 0);
        const std::string name(p.begin() + 7, end);
        m_lastNamedClass=r->classOfDevice;
        m_lastNameModel=p[0]?0:name=="Nintendo RVL-CNT-01"?1:name=="Nintendo RVL-CNT-01-TR"?2:name=="Nintendo RVL-CNT-01-UC"?3:4;
        if (p[0] || !IsWiiName(name)) {
            r->connecting = false;++m_nameRejected;
            // Do not let the same nearby non-Wii device consume every scan.
            // Failed lookups are not cached, so temporary radio errors can retry.
            if(!p[0] && (m_rejectedNames.size()<16 || m_rejectedNames.contains(r->address)))m_rejectedNames[r->address]=m_now+60000;
            return;
        }
        ++m_nameAccepted;
        r->name = name; r->progress = m_now;
        r->incoming=false;
        Bytes data(r->address.begin(), r->address.end()); Put16(data, 0xcc18);
        data.insert(data.end(), {1, 0, 0, 0, 1}); Command(0x0405, data);
    }
    void Connected(std::span<const std::uint8_t> p) {
        if (p.size() != 11) { ++m_malformed; return; }
        auto* r = ByAddress(ReadAddress(p, 3)); if (!r || !IsWiiName(r->name)) return;
        r->connecting = false; if (p[0] || p[9] != 1) {m_nextInquiry=m_now+5000;return;}
        const auto index = size_t(r - m_remotes.data());
        r->handle = Read16(p, 1) & 0xfff; r->progress = m_now;
        r->control.local = static_cast<std::uint16_t>(0x40 + index * 2);
        r->interrupt.local = static_cast<std::uint16_t>(r->control.local + 1);
        if(m_closing) {Bytes b;Put16(b,r->handle);b.push_back(0x13);Command(0x0406,b);return;}
        Authenticate(*r);
    }
    bool Acl(Remote& r, std::uint16_t cid, std::span<const std::uint8_t> data,std::uint64_t dueUs=0) {
        if (!m_aclMtu || data.size() > 1024) return false;
        const auto fragments = (data.size() + 4 + m_aclMtu - 1) / m_aclMtu;
        if (m_acl.size() + fragments > 512) return false;
        Bytes l2; Put16(l2, static_cast<std::uint16_t>(data.size())); Put16(l2, cid); l2.insert(l2.end(), data.begin(), data.end());
        for (size_t pos = 0; pos < l2.size(); pos += m_aclMtu) {
            const auto n = std::min<size_t>(m_aclMtu, l2.size() - pos); Bytes b;
            Put16(b, r.handle | (pos ? 0x1000 : 0x2000)); Put16(b, static_cast<std::uint16_t>(n));
            b.insert(b.end(), l2.begin() + pos, l2.begin() + pos + n); m_acl.push_back({std::move(b),dueUs});
        }
        return true;
    }
    std::uint8_t NextId() { if (!++m_id) ++m_id; return m_id; }
    void SendSignal(Remote& r, std::uint8_t code, std::uint8_t id, std::span<const std::uint8_t> data) {
        Bytes b{code, id}; Put16(b, static_cast<std::uint16_t>(data.size())); b.insert(b.end(), data.begin(), data.end());
        if (!Acl(r, 1, b)) Fail("Bluetooth signaling queue limit reached");
    }
    void OpenChannel(Remote& r, Channel& c, std::uint16_t psm) {
        if (c.peer || c.connectId) return;
        c.connectId = NextId(); Bytes b; Put16(b, psm); Put16(b, c.local); SendSignal(r, 2, c.connectId, b);
    }
    void Configure(Remote& r, Channel& c) {
        if (c.configId || c.outboundConfigured) return;
        c.configId = NextId(); Bytes b; Put16(b, c.peer); Put16(b, 0);
        b.insert(b.end(), {1, 2, 0xb9, 0}); SendSignal(r, 4, c.configId, b);
    }
    void Signal(Remote& r, std::span<const std::uint8_t> p) {
        while (!p.empty()) {
            if (p.size() < 4 || !p[1] || Read16(p, 2) > p.size() - 4) { ++m_malformed; return; }
            const auto code = p[0], id = p[1]; const auto size = Read16(p, 2);
            const auto data = p.subspan(4, size); p = p.subspan(4 + size);
            if (code == 2 && data.size() == 4) {
                const auto psm = Read16(data, 0); auto* c = psm == 0x11 ? &r.control : psm == 0x13 ? &r.interrupt : nullptr;
                const auto peer = Read16(data, 2); const bool valid = c && (psm!=0x13 || r.authenticated) && peer >= 0x40 && (!c->peer || c->peer == peer);
                Bytes b; Put16(b, valid ? c->local : 0); Put16(b, peer); Put16(b, valid ? 0 : 2); Put16(b, 0);
                SendSignal(r, 3, id, b); if (valid) { c->peer = peer; Configure(r, *c); }
            } else if (code == 3 && data.size() == 8) {
                auto* c = Read16(data, 2) == r.control.local ? &r.control : Read16(data, 2) == r.interrupt.local ? &r.interrupt : nullptr;
                if (!c || id != c->connectId) continue;
                const auto result = Read16(data, 4);
                if (result == 1) continue;
                c->connectId = 0;
                if (result == 3) { r.authenticated=false;Authenticate(r);continue; }
                if (result || Read16(data, 0) < 0x40) { Disconnect(r); return; }
                c->peer = Read16(data, 0); Configure(r, *c);
            } else if (code == 4 && data.size() >= 4) {
                auto* c = Read16(data, 0) == r.control.local ? &r.control : Read16(data, 0) == r.interrupt.local ? &r.interrupt : nullptr;
                if (!c || !c->peer) continue;
                const auto flags = Read16(data, 2); Bytes rejected;
                bool malformed = false;
                for (size_t off = 4; off < data.size();) {
                    if (data.size() - off < 2 || size_t(data[off + 1]) > data.size() - off - 2) { malformed = true; break; }
                    const auto n = size_t(data[off + 1]) + 2;
                    if ((data[off] & 0x7f) == 1 && data[off + 1] == 2) {
                        if (Read16(data, off + 2) < 23) rejected.insert(rejected.end(), data.begin() + off, data.begin() + off + n);
                    } else if (!(data[off] & 0x80)) rejected.insert(rejected.end(), data.begin() + off, data.begin() + off + n);
                    off += n;
                }
                if (malformed || (flags & ~1)) { ++m_malformed; continue; }
                Bytes b; Put16(b, c->peer); Put16(b, flags & 1); Put16(b, rejected.empty() ? 0 : 3);
                b.insert(b.end(), rejected.begin(), rejected.end()); SendSignal(r, 5, id, b);
                if (rejected.empty() && !flags) c->inboundConfigured = true;
            } else if (code == 5 && data.size() >= 6) {
                auto* c = Read16(data, 0) == r.control.local ? &r.control : Read16(data, 0) == r.interrupt.local ? &r.interrupt : nullptr;
                if (!c || c->configId != id) continue;
                if (Read16(data, 4) == 4) continue;
                if (Read16(data, 4)) { Disconnect(r); return; }
                if (Read16(data, 2) & 1) { c->configId = 0; Configure(r, *c); continue; }
                c->outboundConfigured = true; c->configId = 0;
            } else if (code == 6 && data.size() == 4) {
                SendSignal(r, 7, id, data); Disconnect(r); return;
            } else if (code == 8) SendSignal(r, 9, id, data);
            else if (code == 10) {
                if (data.size() != 2) { ++m_malformed; continue; }
                Bytes b(data.begin(), data.end()); Put16(b, 1); SendSignal(r, 11, id, b);
            } else if (code != 1 && code != 7 && code != 9 && code != 11) {
                const Bytes reason{0, 0}; SendSignal(r, 1, id, reason);
            }
            if (!r.incoming && r.authenticated && r.control.Ready() && !r.interrupt.peer) OpenChannel(r, r.interrupt, 0x13);
            if (r.Ready()) {
                const auto count=std::count_if(m_remotes.begin(),m_remotes.end(),[](const auto& remote){return remote.Ready();});
                m_status=std::to_string(count)+"/4 USB Wiimotes connected; "+(count==MaxRemotes?"all remote slots occupied":"red SYNC adds another remote automatically");
            }
        }
    }
};
} // namespace UsbBluetooth
