#pragma once
#include <array>
#include <cmath>

// Use the original camera pixels, not the gravity-rotated pointer coordinates.
struct WiimoteIrReference
{
    struct Dot { unsigned x{}, y{}; bool visible{}; };
    std::array<float, 2> horizon{};
    float distance = 0;
    bool valid = false;

    static WiimoteIrReference Read(const Dot& a, const Dot& b,
        const std::array<float, 3>& acceleration)
    {
        WiimoteIrReference result;
        if (!a.visible || !b.visible || a.x >= 1023 || b.x >= 1023 ||
            a.y >= 768 || b.y >= 768)
            return result;
        float x = float(a.x) - float(b.x), y = float(b.y) - float(a.y);
        const float length = std::hypot(x, y);
        if (length < 8) return result;
        x /= length; y /= length;
        // Dot ordering is not stable. Resolve its 180-degree ambiguity with gravity.
        const float projection = std::hypot(acceleration[0], acceleration[1]);
        if (!std::isfinite(projection) || projection < 0.25f) return result;
        if (x * -acceleration[1] + y * acceleration[0] < 0) { x = -x; y = -y; }
        // KPAD reports the pointer plane in screen-right/down coordinates.
        // Its roll is the inverse of the sensor-bar line seen by the camera.
        result.horizon = {x, -y};
        // Approximate range in meters for the standard 20cm sensor bar / 42deg camera.
        result.distance = 0.2f * 1023.0f / (2 * 0.38386404f * length);
        result.valid = true;
        return result;
    }
};
