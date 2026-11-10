#pragma once

#include <cstdint>
#include <optional>

namespace padscore
{
// Guest control handshake only. Streaming stays not-ready until an audio sink exists.
struct WpadSpeakerControl
{
    bool enabled = false;
    bool muted = true;
    bool playing = false;
    std::optional<uint32_t> pending;

    int32_t Queue(bool connected, uint32_t command)
    {
        if (!connected) return -1;
        if (command > 5) return -4;
        if (pending) return -2;
        pending = command;
        return 0;
    }

    std::optional<int32_t> Complete(bool connected)
    {
        const auto command = pending;
        pending.reset();
        if (!connected)
        {
            enabled = playing = false;
            muted = true;
            return command ? std::optional<int32_t>{-1} : std::nullopt;
        }
        if (!command) return std::nullopt;
        switch (*command)
        {
        case 0: enabled = playing = false; muted = true; break;
        case 1:
        case 5: enabled = true; playing = false; muted = true; break;
        case 2: muted = true; break;
        case 3: muted = false; break;
        case 4: playing = enabled; muted = false; break;
        }
        return 0;
    }
};
}
