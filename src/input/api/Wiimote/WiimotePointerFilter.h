#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// Filter only the screen pointer, never the raw IR dots used by MotionPlus.
class WiimotePointerFilter
{
public:
    void Reset() { m_valid = false; }

    std::array<float, 2> Update(float x, float y, float elapsedSeconds)
    {
        if (!m_valid)
        {
            m_valid = true;
            m_position = {x, y};
            return m_position;
        }
        const float distance = std::hypot(x - m_position[0], y - m_position[1]);
        // Suppress hand/camera noise near rest, but pass deliberate movements
        // immediately. A time-based coefficient avoids report-rate dependence.
        const float response = std::clamp(distance / 0.02f, 0.0f, 1.0f);
        const float tau = 0.025f * (1.0f - response);
        const float dt = std::clamp(elapsedSeconds, 0.001f, 0.1f);
        const float alpha = tau > 0.0001f ? 1.0f - std::exp(-dt / tau) : 1.0f;
        m_position[0] += alpha * (x - m_position[0]);
        m_position[1] += alpha * (y - m_position[1]);
        return m_position;
    }

private:
    bool m_valid = false;
    std::array<float, 2> m_position{};
};
