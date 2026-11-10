#pragma once

#include <api/Wiimote/WiimoteDevice.h>
#include <memory>
#ifdef _WIN32
class WindowsSpeakerWriter;
#endif

class HidapiWiimote : public WiimoteDevice {
public:
    HidapiWiimote(struct SDL_hid_device* dev, std::string_view path);
    ~HidapiWiimote() override;

    bool write_data(const std::vector<uint8> &data) override;
    bool write_audio_data(const std::vector<uint8>& data,bool compatibility) override;
    std::optional<std::vector<uint8>> read_data() override;
    bool operator==(const WiimoteDevice& o) const override;

    static std::vector<WiimoteDevicePtr> get_devices();

private:
    struct SDL_hid_device* m_handle;
    const std::string m_path;
#ifdef _WIN32
    std::unique_ptr<WindowsSpeakerWriter> m_speaker_writer;
#endif

};
