#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace WiimoteReportRecovery
{
    constexpr bool Ready(std::uint8_t report,bool recovery,bool memoryPending,bool controlReady)
    {
        return report==0x12 && recovery && !memoryPending && controlReady;
    }
    template<class Queue>
    void Insert(Queue& queue,std::size_t device,std::vector<std::uint8_t> bytes)
    {
        // Match accepted input ordering: never jump over extension reads/setup.
        queue.emplace_back(device,std::move(bytes),0,0,true);
    }
}
