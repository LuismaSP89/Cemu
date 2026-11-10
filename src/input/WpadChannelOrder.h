#pragma once
#include <array>
#include <memory>
#include <utility>

namespace input
{
    // Logical Cemu slots include the GamePad. WPAD channels do not. Give a
    // newly live controller the first free channel without moving live players.
    template<class T, size_t N, class IsLive>
    void ReconcileWpadChannels(std::array<std::shared_ptr<T>, N>& pads,
        std::array<std::weak_ptr<T>, N>& previous, IsLive live)
    {
        for (size_t i = 0; i < N; ++i)
        {
            if (!live(pads[i])) continue;
            bool known = false;
            for (const auto& old : previous) known |= old.lock() == pads[i];
            if (known) continue;
            for (size_t free = 0; free < i; ++free)
                if (!live(pads[free])) { std::swap(pads[free], pads[i]); break; }
        }
        for (size_t i = 0; i < N; ++i) previous[i] = live(pads[i]) ? pads[i] : nullptr;
    }
}
