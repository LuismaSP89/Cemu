#include "input/api/Wiimote/hidapi/HidapiWiimote.h"
#include <SDL3/SDL.h>
#include <cwchar>
#include "WindowsSpeakerWriter.h"

static constexpr uint16 WIIMOTE_VENDOR_ID = 0x057e;
static constexpr uint16 WIIMOTE_PRODUCT_ID = 0x0306;
static constexpr uint16 WIIMOTE_MP_PRODUCT_ID = 0x0330;
static constexpr uint16 WIIMOTE_MAX_INPUT_REPORT_LENGTH = 22;
static constexpr auto PRO_CONTROLLER_NAME = L"Nintendo RVL-CNT-01-UC";

HidapiWiimote::HidapiWiimote(SDL_hid_device* dev, std::string_view path)
 : m_handle(dev), m_path(path) {
}

bool HidapiWiimote::write_data(const std::vector<uint8> &data) {
    bool drained=true;
#ifdef _WIN32
    if(m_speaker_writer)
    {
        drained=m_speaker_writer->Drain();
        if(std::getenv("CEMU_WIIMOTE_SPEAKER_TRACE"))
            cemuLog_log(LogType::Force,"Wiimote queued speaker completions: attempted={} completed={} failed={}",m_speaker_writer->Attempted(),m_speaker_writer->Completed(),m_speaker_writer->Failed());
    }
#endif
    const bool written=SDL_hid_write(m_handle, data.data(), data.size()) >= 0;
    return drained && written;
}

bool HidapiWiimote::write_audio_data(const std::vector<uint8>& data,bool compatibility)
{
#ifdef _WIN32
    if(compatibility)
    {
        try
        {
            if(!m_speaker_writer)m_speaker_writer=std::make_unique<WindowsSpeakerWriter>(m_path);
            return m_speaker_writer->Submit(data);
        }
        catch(const std::exception& error){cemuLog_log(LogType::Force,"Wiimote queued speaker error: {}",error.what());return false;}
    }
#endif
    return write_data(data);
}

std::optional<std::vector<uint8>> HidapiWiimote::read_data() {
    std::array<uint8, WIIMOTE_MAX_INPUT_REPORT_LENGTH> read_data{};
    const auto result = SDL_hid_read(m_handle, read_data.data(), WIIMOTE_MAX_INPUT_REPORT_LENGTH);
    if (result < 0)
        return {};
    return {{read_data.cbegin(), read_data.cbegin() + result}};
}

std::vector<WiimoteDevicePtr> HidapiWiimote::get_devices() {
    std::vector<WiimoteDevicePtr> wiimote_devices;
    const auto initResult = SDL_hid_init();
    const auto device_enumeration = SDL_hid_enumerate(WIIMOTE_VENDOR_ID, 0x0);
    size_t enumerated = 0, supported = 0, failed = 0;
    std::string openError;
    for (auto it = device_enumeration; it != nullptr; it = it->next){
        ++enumerated;
        if (it->product_id != WIIMOTE_PRODUCT_ID && it->product_id != WIIMOTE_MP_PRODUCT_ID)
            continue;
        if (it->product_string && std::wcscmp(it->product_string, PRO_CONTROLLER_NAME) == 0)
            continue;
        ++supported;
        auto dev = SDL_hid_open_path(it->path);
        if (!dev){
            ++failed;
            openError = SDL_GetError();
            cemuLog_logDebug(LogType::Force, "Unable to open Wiimote device at {}: {}", it->path, openError);
        }
        else {
            SDL_hid_set_nonblocking(dev, true);
            wiimote_devices.push_back(std::make_shared<HidapiWiimote>(dev, it->path));
        }
    }
    // Private transport diagnostics are opt-in and emit only when discovery changes.
    if (std::getenv("CEMU_WIIMOTE_TRANSPORT_TRACE")) {
        const auto status = fmt::format("init={} enumerated={} supported={} opened={} failed={} error={}",
            initResult, enumerated, supported, wiimote_devices.size(), failed, openError);
        static std::string previous;
        if (status != previous) {
            cemuLog_log(LogType::Force, "Wiimote HID discovery: {}", status);
            previous = status;
        }
    }
    SDL_hid_free_enumeration(device_enumeration);
    return wiimote_devices;
}

bool HidapiWiimote::operator==(const WiimoteDevice& rhs) const {
    auto other = dynamic_cast<const HidapiWiimote*>(&rhs);
    if (!other)
        return false;
    return m_path == other->m_path;
}

HidapiWiimote::~HidapiWiimote() {
#ifdef _WIN32
    m_speaker_writer.reset();
#endif
    SDL_hid_close(m_handle);
}
