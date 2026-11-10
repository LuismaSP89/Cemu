#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace UsbBluetooth {
struct AdapterId {
    std::uint16_t vendor{}, product{};
    bool operator==(const AdapterId&) const = default;
};
inline std::optional<AdapterId> ParseAdapterId(std::string_view text) {
    if (text.size() != 9 || text[4] != ':') return {};
    unsigned values[2]{};
    for (unsigned field = 0; field < 2; ++field) for (unsigned digit = 0; digit < 4; ++digit) {
        const char c = text[field * 5 + digit];
        const int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (value < 0) return {};
        values[field] = values[field] * 16 + value;
    }
    if (!values[0] || values[0] == 0xffff) return {};
    return AdapterId{static_cast<std::uint16_t>(values[0]), static_cast<std::uint16_t>(values[1])};
}
inline std::optional<std::size_t> UniqueAdapter(std::span<const AdapterId> devices, AdapterId selected) {
    std::optional<std::size_t> match;
    for (std::size_t i = 0; i < devices.size(); ++i) if (devices[i] == selected) {
        if (match) return {};
        match = i;
    }
    return match;
}
struct UsbEndpoint { std::uint8_t address{}, type{}; };
struct HciEndpoints { std::uint8_t eventIn{}, aclIn{}, aclOut{}; };
inline bool ClassicHciClass(unsigned type, unsigned subclass, unsigned protocol) {
    return type == 0xe0 && subclass == 1 && protocol == 1;
}
inline std::optional<HciEndpoints> StandardHciEndpoints(bool deviceHci, unsigned type, unsigned subclass,
    unsigned protocol, unsigned number, unsigned alternate, std::span<const UsbEndpoint> endpoints) {
    if (number != 0 || alternate != 0 || (!deviceHci && !ClassicHciClass(type, subclass, protocol)) ||
        (type != 0 && !ClassicHciClass(type, subclass, protocol))) return {};
    HciEndpoints result;
    unsigned events = 0, inputs = 0, outputs = 0;
    for (const auto& ep : endpoints) {
        if (!(ep.address & 15) || (ep.address & 0x70)) return {};
        if ((ep.type & 3) == 3 && (ep.address & 0x80)) { result.eventIn = ep.address; ++events; }
        else if ((ep.type & 3) == 2 && (ep.address & 0x80)) { result.aclIn = ep.address; ++inputs; }
        else if ((ep.type & 3) == 2 && !(ep.address & 0x80)) { result.aclOut = ep.address; ++outputs; }
        else return {};
    }
    return events == 1 && inputs == 1 && outputs == 1 ? std::optional(result) : std::nullopt;
}
struct SpeakerOptions { bool pcm = true; unsigned rate = 3000, gain = 2; };
inline SpeakerOptions ResolveSpeakerOptions(std::string_view mode, std::string_view rate, std::string_view gain) {
    SpeakerOptions result;
    if (mode == "0") result.pcm = false;
    if (rate == "2000") result.rate = 2000;
    else if (rate == "2500") result.rate = 2500;
    if (gain == "1") result.gain = 1;
    return result;
}
} // namespace UsbBluetooth
