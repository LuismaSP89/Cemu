#include "input/api/Wiimote/usb/UsbBluetoothHost.h"
#include "input/api/Wiimote/usb/UsbBluetoothProtocol.h"
#include "input/api/Wiimote/usb/UsbBluetoothTrace.h"
#include "input/api/Wiimote/usb/UsbBluetoothBonds.h"
#include "input/api/Wiimote/usb/UsbBluetoothPcm.h"
#include "input/api/Wiimote/usb/UsbBluetoothAdapter.h"
#include "input/api/Wiimote/WiimoteOutputWait.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "util/helpers/helpers.h"

#include <libusb.h>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {
using namespace UsbBluetooth;
std::uint64_t Milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::uint64_t Microseconds() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::filesystem::path TracePath() {
#ifdef _WIN32
    const auto* path = _wgetenv(L"CEMU_USB_BLUETOOTH_TRACE");
#else
    const auto* path = std::getenv("CEMU_USB_BLUETOOTH_TRACE");
#endif
    return path ? std::filesystem::path(path) : std::filesystem::path{};
}
struct Shared {
    std::mutex mutex;
    Session session;
    bool online = false;
    SpeakerOptions speaker = [] {
        const auto env = [](const char* name) { const auto* value = std::getenv(name); return value ? std::string_view(value) : std::string_view{}; };
        return ResolveSpeakerOptions(env("CEMU_USB_SPEAKER_PCM"), env("CEMU_USB_PCM_RATE"), env("CEMU_USB_SPEAKER_GAIN"));
    }();
    bool pcm = speaker.pcm;
    unsigned speakerGain = speaker.gain, pcmRate = speaker.rate;
    std::string adapterId;
    std::map<Address,PcmSpeaker> speakers;
    std::string status = "Dedicated USB Bluetooth disabled";
};

class UsbRemote final : public WiimoteDevice {
public:
    UsbRemote(std::shared_ptr<Shared> state, Address address) : m_state(state), m_address(address) {}
    bool write_data(const std::vector<uint8>& b) override {
        auto state = m_state.lock(); if (!state) return false;
        std::scoped_lock lock(state->mutex);
        if(!state->online) return false;
        if(!state->pcm) return state->session.WriteReport(m_address,b);
        auto next=state->speakers[m_address];
        next.SetGain(state->speakerGain);
        next.SetRateLimit(state->pcmRate);
        const auto reports=next.Translate(b,Microseconds());
        if(!next.Accepted() || (!reports.empty() && !state->session.WriteReports(m_address,reports))) return false;
        state->speakers[m_address]=std::move(next);return true;
    }
    std::optional<std::vector<uint8>> read_data() override {
        auto state = m_state.lock(); if (!state) return {};
        std::scoped_lock lock(state->mutex);
        return state->online ? state->session.ReadReport(m_address) : std::nullopt;
    }
    bool operator==(const WiimoteDevice& other) const override {
        const auto* device = dynamic_cast<const UsbRemote*>(&other);
        return device && device->m_state.lock() == m_state.lock() && device->m_address == m_address;
    }
private:
    std::weak_ptr<Shared> m_state;
    Address m_address;
};

struct Connection {
    struct Slot {
        Connection* owner{};
        libusb_transfer* transfer{};
        std::array<unsigned char, 4096> buffer{};
        unsigned kind{};
        bool active = false;
        bool speaker = false;
        std::uint64_t submittedUs{},dueUs{};
    };
    std::shared_ptr<Shared> state;
    SessionTrace& trace;
    libusb_context* context{};
    libusb_device_handle* handle{};
    unsigned char eventIn{}, aclIn{}, aclOut{};
    std::array<Slot, 4> slots{};
    Bytes eventBytes, aclBytes;
    bool running = false, failed = false, claimed = false, identityLogged = false;
    bool bondsLoaded=false,bondsWritable=false;
    unsigned bondRevision=0;
    OutputTiming timing;
    WiimoteOutputWait outputWait;
    std::uint64_t nextTiming=0;
    const std::filesystem::path legacyBondPath=ActiveSettings::GetConfigPath("controllerProfiles/usb-bluetooth-bonds.bin");
    std::filesystem::path bondPath;

    explicit Connection(std::shared_ptr<Shared> shared, SessionTrace& sessionTrace) : state(std::move(shared)), trace(sessionTrace) {}
    void Status(std::string text) {
        {
            std::scoped_lock lock(state->mutex);
            if (state->status == text) return;
            state->status = text;
        }
        trace.Line("status: " + text);
        cemuLog_log(LogType::Force, "USB Bluetooth: {}", text);
    }
    bool Open() {
        std::string selection;
        { std::scoped_lock lock(state->mutex); selection = state->adapterId; }
        const auto adapter = ParseAdapterId(selection);
        if (!adapter) { Status("Invalid dedicated Bluetooth USB ID; use four hexadecimal digits:four hexadecimal digits"); return false; }
        if (libusb_init(&context) != LIBUSB_SUCCESS) { Status("USB Bluetooth initialization failed"); return false; }
        libusb_device** list{};
        const auto count = libusb_get_device_list(context, &list);
        if (count < 0) { Status("USB Bluetooth device enumeration failed"); return false; }
        libusb_device* selected = nullptr;
        unsigned matches = 0;
        for (std::ptrdiff_t i = 0; i < count; ++i) {
            libusb_device_descriptor d{};
            if (libusb_get_device_descriptor(list[i], &d) == 0 && AdapterId{d.idVendor, d.idProduct} == *adapter) {
                selected = list[i]; ++matches;
            }
        }
        if (matches != 1) {
            libusb_free_device_list(list, 1);
            Status(matches ? "Multiple selected USB adapters found; refusing ambiguous selection" : "Selected USB adapter " + selection + " is not accessible; verify its connection and WinUSB driver. No other adapter will be used.");
            return false;
        }
        libusb_config_descriptor* config{};
        const int result = libusb_get_active_config_descriptor(selected, &config);
        if (result != 0) {
            libusb_free_device_list(list, 1); Status("Selected adapter has no active USB configuration; verify its driver"); return false;
        }
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(selected, &descriptor) != 0) { libusb_free_config_descriptor(config); libusb_free_device_list(list, 1); Status("Cannot read selected adapter descriptor"); return false; }
        const bool deviceHci = ClassicHciClass(descriptor.bDeviceClass, descriptor.bDeviceSubClass, descriptor.bDeviceProtocol);
        for (unsigned i = 0; i < config->bNumInterfaces; ++i) {
            for (int a = 0; a < config->interface[i].num_altsetting; ++a) {
                const auto& iface = config->interface[i].altsetting[a];
                std::vector<UsbEndpoint> endpoints;
                for (unsigned e = 0; e < iface.bNumEndpoints; ++e) {
                    const auto& ep = iface.endpoint[e];
                    endpoints.push_back({ep.bEndpointAddress, ep.bmAttributes});
                }
                if (const auto selectedEndpoints = StandardHciEndpoints(deviceHci, iface.bInterfaceClass, iface.bInterfaceSubClass,
                    iface.bInterfaceProtocol, iface.bInterfaceNumber, iface.bAlternateSetting, endpoints)) {
                    eventIn = selectedEndpoints->eventIn; aclIn = selectedEndpoints->aclIn; aclOut = selectedEndpoints->aclOut;
                }
            }
        }
        libusb_free_config_descriptor(config);
        if (!eventIn || !aclIn || !aclOut) { libusb_free_device_list(list, 1); Status("Selected adapter is not a supported standard Classic Bluetooth USB HCI interface"); return false; }
        const int opened = libusb_open(selected, &handle);
        libusb_free_device_list(list, 1);
        if (opened != 0) { Status("Cannot open selected adapter " + selection + " with WinUSB; no other adapter will be used"); return false; }
        // Never detach a Windows driver or select a different USB device as a fallback.
        if (libusb_claim_interface(handle, 0) != 0) { Status("Selected interface is unavailable or already owned; close other software using that dongle"); return false; }
        claimed = true;
        for (unsigned i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i]; slot.owner = this; slot.kind = i; slot.transfer = libusb_alloc_transfer(0);
            if (!slot.transfer) { Status("USB Bluetooth transfer allocation failed"); return false; }
        }
        running = true;
        {
            std::scoped_lock lock(state->mutex);
            state->session.Start(Milliseconds()); state->online = true;
            state->speakers.clear();
        }
        libusb_fill_interrupt_transfer(slots[0].transfer, handle, eventIn, slots[0].buffer.data(), 257, Callback, &slots[0], 0);
        libusb_fill_bulk_transfer(slots[1].transfer, handle, aclIn, slots[1].buffer.data(), 4096, Callback, &slots[1], 0);
        Submit(slots[0]); Submit(slots[1]);
        Status("Selected adapter " + selection + " opened exclusively; reading HCI capabilities (no firmware upload)");
        trace.Line(state->pcm?"USB-only PCM8 speaker output enabled; resampled wire rate cap="+std::to_string(state->pcmRate)+"Hz, guest/HID/DolphinBar unchanged":"USB speaker output uses original ADPCM reports");
        if(state->pcm)trace.Line("USB PCM gain="+std::to_string(state->speakerGain)+"; gain2 uses soft limiting; USB-local high-resolution pacing while streaming");
        return !failed;
    }
    static void LIBUSB_CALL Callback(libusb_transfer* t) {
        auto& slot = *static_cast<Slot*>(t->user_data); auto& c = *slot.owner;
        slot.active = false;
        if(slot.kind==3 && slot.speaker && c.running) c.timing.Complete(Microseconds()-slot.submittedUs,t->status==LIBUSB_TRANSFER_COMPLETED);
        if (t->status == LIBUSB_TRANSFER_COMPLETED) {
            if (slot.kind < 2 && t->actual_length > 0 && c.running) {
                c.Feed(slot.kind == 0, std::span(slot.buffer.data(), size_t(t->actual_length)));
                if (!c.failed) c.Submit(slot);
            }
        } else if (t->status != LIBUSB_TRANSFER_CANCELLED && c.running) c.failed = true;
    }
    void Submit(Slot& slot) {
        slot.submittedUs=Microseconds();
        if (libusb_submit_transfer(slot.transfer) == 0) {slot.active = true;if(slot.kind==3 && slot.speaker){++timing.submitted;timing.Deadline(slot.submittedUs,slot.dueUs);}}
        else failed = true;
    }
    void Feed(bool events, std::span<const std::uint8_t> bytes) {
        auto& pending = events ? eventBytes : aclBytes;
        if (pending.size() + bytes.size() > 8192) { failed = true; return; }
        pending.insert(pending.end(), bytes.begin(), bytes.end());
        const size_t header = events ? 2 : 4;
        size_t used = 0;
        std::scoped_lock lock(state->mutex);
        while (pending.size() - used >= header) {
            const auto view = std::span(pending).subspan(used);
            const size_t n = header + (events ? view[1] : Read16(view, 2));
            if (n > (events ? 257u : 2048u)) { failed = true; break; }
            if (view.size() < n) break;
            if (events) {
                trace.Event(view.first(n));
                state->session.Event(view.first(n));
                if(!bondsLoaded && n>=12 && view[0]==0x0e && Read16(view,3)==0x1009 && view[5]==0) {
                    bondsLoaded=true;
                    bondPath=AdapterBondPath(legacyBondPath,state->session.LocalAddress());
                    const auto bonds=LoadAdapterBonds(legacyBondPath,state->session.LocalAddress());
                    bondsWritable=bonds && state->session.RestoreBonds(*bonds);
                    trace.Line(bondsWritable?"Stored USB bond cache loaded; remotes="+std::to_string(bonds->size()):"USB bond cache unreadable or mismatched; using session pairing only, file preserved");
                }
                if (!identityLogged && n >= 14 && view[0] == 0x0e && Read16(view, 3) == 0x1001 && view[5] == 0) {
                    identityLogged = true;
                    cemuLog_log(LogType::Force, "USB Bluetooth capabilities: HCI={} manufacturer={} LMP_subversion=0x{:04x}; no firmware upload",
                        state->session.HciVersion(), state->session.Manufacturer(), state->session.Subversion());
                }
            } else {
                if(trace.Enabled() && n>=12 && ((Read16(view,0)>>12)&3)!=1 && Read16(view,6)==1) {
                    trace.Line("L2CAP signaling RX code="+std::to_string(view[8])+" length="+std::to_string(Read16(view,10))+
                        ((view[8]==3 && n>=20)?" result="+std::to_string(Read16(view,16)):""));
                }
                if (trace.Enabled() && n >= 9 && ((Read16(view, 0) >> 12) & 3) != 1) {
                    const auto handle = Read16(view, 0) & 0xfff;
                    for (const auto& remote : state->session.Remotes()) {
                        if (remote.handle == handle && remote.control.local == Read16(view, 6)) trace.HidControl(handle, view[8]);
                    }
                }
                state->session.AclInput(view.first(n));
            }
            used += n;
        }
        pending.erase(pending.begin(), pending.begin() + used);
    }
    bool Step() {
        std::optional<Bytes> command, acl;
        std::optional<std::vector<Bond>> newBonds;
        Address host{};
        bool timedAudio=false;
        std::uint64_t aclDueUs{};
        std::string status,timingLine,discoveryLine;
        {
            std::scoped_lock lock(state->mutex);
            state->session.Tick(Milliseconds());
            if(state->online && state->pcm && state->session.Ready()) {
                for(auto& [address,speaker]:state->speakers) if(speaker.Buffered()) {
                    auto next=speaker;
                    auto reports=next.Flush(Microseconds());
                    if(!reports.empty() && state->session.WriteReports(address,reports)) speaker=std::move(next);
                }
            }
            if(trace.Enabled()) {
                timing.ObserveQueue(state->session.QueuedAcl(),state->session.QueuedAcl() && state->session.InflightAcl()>=state->session.AclLimit());
                if(Milliseconds()>=nextTiming) {
                    nextTiming=Milliseconds()+5000;
                    timingLine=timing.Summary(state->session.AudioQueued(),state->session.AudioRejected(),state->session.InflightAcl(),state->session.AclLimit());
                    discoveryLine=state->session.DiscoverySummary();
                }
            }
            status = state->session.Status();
            if (!state->session.Error().empty()) { status = state->session.Error(); failed = true; }
            if (!failed && !slots[2].active) command = state->session.TakeCommand();
            if (!failed && !slots[3].active) {acl = state->session.TakeAcl(Microseconds());aclDueUs=state->session.LastAclDueUs();}
            timedAudio=state->pcm && state->session.AudioActive();
            if(bondsWritable && bondRevision!=state->session.BondRevision()) {
                bondRevision=state->session.BondRevision();host=state->session.LocalAddress();
                auto bonds=state->session.Bonds();if(!bonds.empty()) newBonds=std::move(bonds);
            }
        }
        if(!timingLine.empty()) trace.Line(timingLine);
        if(!discoveryLine.empty()) trace.Line(discoveryLine);
        if(newBonds) trace.Line(SaveBonds(bondPath,host,*newBonds)?"Authenticated USB bonds saved with Windows user encryption; remotes="+std::to_string(newBonds->size()):"USB bond save failed; current session remains usable");
        Status(std::move(status));
        if (failed) return false;
        if (command) {
            trace.Command(*command);
            auto& slot = slots[2];
            libusb_fill_control_setup(slot.buffer.data(), 0x20, 0, 0, 0, static_cast<std::uint16_t>(command->size()));
            std::copy(command->begin(), command->end(), slot.buffer.begin() + LIBUSB_CONTROL_SETUP_SIZE);
            libusb_fill_control_transfer(slot.transfer, handle, slot.buffer.data(), Callback, &slot, 1000);
            Submit(slot);
        }
        if (acl) {
            auto& slot = slots[3]; std::copy(acl->begin(), acl->end(), slot.buffer.begin());
            if(trace.Enabled() && acl->size()>=12 && Read16(*acl,6)==1) trace.Line("L2CAP signaling TX code="+std::to_string((*acl)[8])+" length="+std::to_string(Read16(*acl,10)));
            slot.speaker=acl->size()>=10 && (*acl)[8]==0xa2 && (*acl)[9]==0x18;
            slot.dueUs=aclDueUs;
            libusb_fill_bulk_transfer(slot.transfer, handle, aclOut, slot.buffer.data(), static_cast<int>(acl->size()), Callback, &slot, 1000);
            Submit(slot);
        }
        // Don't use a coarse libusb timeout to clock10ms audio packets.
        timeval wait{0, timedAudio?0:2000};
        const auto result = libusb_handle_events_timeout(context, &wait);
        if (result < 0 && result != LIBUSB_ERROR_INTERRUPTED) failed = true;
        if(timedAudio && !failed)outputWait.WaitUntil(std::chrono::steady_clock::now()+std::chrono::microseconds(500),true);
        if (failed) {
            std::string error;
            { std::scoped_lock lock(state->mutex); if (state->session.Error().empty()) state->session.TransportFailed(); error = state->session.Error(); }
            Status(std::move(error));
        }
        return !failed;
    }
    bool Close() {
        {std::scoped_lock lock(state->mutex);trace.Line(timing.Summary(state->session.AudioQueued(),state->session.AudioRejected(),state->session.InflightAcl(),state->session.AclLimit()));}
        trace.Line("USB transport closing; input marked offline");
        { std::scoped_lock lock(state->mutex); state->online = false; }
        if(running && !failed) {
            { std::scoped_lock lock(state->mutex);state->session.BeginShutdown(); }
            const auto deadline=Milliseconds()+750;
            while(Milliseconds()<deadline) {
                bool pending;
                {std::scoped_lock lock(state->mutex);pending=state->session.ShutdownPending();}
                if(!pending || !Step()) break;
            }
            {std::scoped_lock lock(state->mutex);trace.Line(state->session.ShutdownPending()?"USB disconnect grace expired; bounded cancellation follows":"USB remote disconnects completed before adapter release");}
        }
        running = false;
        for (auto& slot : slots) if (slot.active) libusb_cancel_transfer(slot.transfer);
        const auto until = Milliseconds() + 2000;
        const auto active = [&] { return std::any_of(slots.begin(), slots.end(), [](const auto& s) { return s.active; }); };
        while (active() && Milliseconds() < until) {
            timeval wait{0, 10000}; libusb_handle_events_timeout(context, &wait);
        }
        if (active()) return false;
        for (auto& slot : slots) if (slot.transfer) { libusb_free_transfer(slot.transfer); slot.transfer = nullptr; }
        if (claimed) libusb_release_interface(handle, 0);
        if (handle) { libusb_close(handle); handle = nullptr; }
        if (context) { libusb_exit(context); context = nullptr; }
        return true;
    }
};
} // namespace

struct UsbBluetoothHost::Impl {
    std::shared_ptr<Shared> state = std::make_shared<Shared>();
    SessionTrace trace{TracePath()};
    std::atomic_bool enabled = false, stop = false;
    std::atomic_uint64_t wake = 0;
    std::mutex waitMutex;
    std::condition_variable condition;
    std::thread worker;
    std::mutex shutdownMutex;

    Impl() {
        state->adapterId = GetConfig().wiimote_usb_adapter.GetValue();
        worker = std::thread([this] { Run(); });
    }
    ~Impl() { Shutdown(); }
    void Shutdown() { std::scoped_lock lock(shutdownMutex);stop=true;condition.notify_all();if(worker.joinable())worker.join(); }
    void Wait(std::uint64_t epoch, bool retry) {
        std::unique_lock lock(waitMutex);
        if (retry) condition.wait_for(lock, std::chrono::seconds(3), [&] { return stop || !enabled || wake != epoch; });
        else condition.wait(lock, [&] { return stop || (enabled && wake != epoch); });
    }
    void Run() {
        SetThreadName("USB-Bluetooth");
        while (!stop) {
            const auto epoch = wake.load();
            if (!enabled) { Wait(epoch, false); continue; }
            auto connection = std::make_unique<Connection>(state, trace);
            const bool opened = connection->Open();
            if (opened) {
                while (!stop && enabled && connection->Step()) {}
            }
            if (!connection->Close()) {
                connection->Status("USB cancellation did not complete; restart this private Cemu before retrying");
                // Retain outstanding callback storage instead of freeing it while Windows may still reference it.
                connection.release(); enabled = false;
                std::unique_lock lock(waitMutex); condition.wait(lock, [&] { return stop.load(); });
                return;
            }
            if (!enabled) {
                std::scoped_lock lock(state->mutex); state->status = "Dedicated USB Bluetooth disabled";
            }
            if (!stop) Wait(wake.load(), !opened && enabled);
        }
    }
};

UsbBluetoothHost::UsbBluetoothHost() : m_impl(std::make_unique<Impl>()) {}
UsbBluetoothHost::~UsbBluetoothHost() = default;
void UsbBluetoothHost::Shutdown() { m_impl->Shutdown(); }
void UsbBluetoothHost::SetEnabled(bool enabled) {
    if(m_impl->stop) return;
    m_impl->enabled = enabled; ++m_impl->wake; m_impl->condition.notify_all();
}
bool UsbBluetoothHost::Enabled() const { return m_impl->enabled.load(); }
bool UsbBluetoothHost::SetAdapterId(std::string id) {
    if (!ParseAdapterId(id) || m_impl->enabled || m_impl->stop) return false;
    std::scoped_lock lock(m_impl->state->mutex);
    m_impl->state->adapterId = std::move(id);
    return true;
}
void UsbBluetoothHost::RequestSync() {
    { std::scoped_lock lock(m_impl->state->mutex); m_impl->state->session.RequestSync(); }
    ++m_impl->wake; m_impl->condition.notify_all();
}
std::string UsbBluetoothHost::Status() const { std::scoped_lock lock(m_impl->state->mutex); return m_impl->state->status; }
std::vector<WiimoteDevicePtr> UsbBluetoothHost::Devices() {
    std::vector<WiimoteDevicePtr> out;
    std::scoped_lock lock(m_impl->state->mutex);
    if (m_impl->state->online) for (const auto& r : m_impl->state->session.Remotes()) {
        if (r.Ready()) out.push_back(std::make_shared<UsbRemote>(m_impl->state, r.address));
    }
    return out;
}
