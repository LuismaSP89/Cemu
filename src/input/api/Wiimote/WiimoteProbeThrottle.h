#pragma once
#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

// Called under the registration mutex, never under the audio/device-registry lock.
template<class Device> class WiimoteProbeThrottle
{
public:
    using Clock=std::chrono::steady_clock;
    bool Ready(const std::shared_ptr<Device>& device, Clock::time_point now)
    {
        std::erase_if(m_failures, [](const auto& entry){return entry.device.expired();});
        for(const auto& entry:m_failures)
            if(const auto previous=entry.device.lock(); previous && *previous==*device)
                return now>=entry.retry;
        return true;
    }
    void Failed(const std::shared_ptr<Device>& device, Clock::time_point now)
    {
        Succeeded(device);
        m_failures.push_back({device,now+std::chrono::milliseconds(500)});
    }
    void Succeeded(const std::shared_ptr<Device>& device)
    {
        std::erase_if(m_failures,[&](const auto& entry){
            const auto previous=entry.device.lock();
            return !previous || *previous==*device;
        });
    }
private:
    struct Failure {std::weak_ptr<Device> device;Clock::time_point retry;};
    std::vector<Failure> m_failures;
};
