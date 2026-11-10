#pragma once

#include "input/api/Wiimote/WiimoteDevice.h"
#include <memory>
#include <string>

class UsbBluetoothHost {
public:
    UsbBluetoothHost();
    ~UsbBluetoothHost();
    void Shutdown();
    void SetEnabled(bool enabled);
    bool Enabled() const;
    bool SetAdapterId(std::string id);
    void RequestSync();
    std::string Status() const;
    std::vector<WiimoteDevicePtr> Devices();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
