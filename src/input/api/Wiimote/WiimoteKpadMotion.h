#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include "input/api/Wiimote/WiimoteIrReference.h"

// Physical Wiimote pose, independent of the GamePad/SDL fusion conventions.
class WiimoteKpadMotion
{
public:
    using Vec = std::array<float, 3>;
    using Matrix = std::array<Vec, 3>;
    struct Sample
    {
        Vec gyro{}, angle{};
        Vec acceleration{};
        WiimoteIrReference ir;
        Matrix dir{{{1,0,0},{0,1,0},{0,0,1}}};
        bool ready = false;
    };

    void Update(float dt, const Vec& parser_gyro, const Vec& acceleration, bool gyro_ready = true)
    {
        // Parser order is signed pitch/roll/yaw. KPAD is pitch/yaw/roll;
        // its acceleration basis is (-raw.x, -raw.z, raw.y).
        const Vec omega{-parser_gyro[0], -parser_gyro[2], -parser_gyro[1]};
        for (float v : omega) if (!std::isfinite(v)) return;
        for (unsigned i = 0; i < 3; ++i)
        {
            m_sample.gyro[i] = omega[i] / Tau;
            if (std::abs(m_sample.gyro[i]) < 0.00025f) m_sample.gyro[i] = 0;
        }
        const float length = std::sqrt(Dot(acceleration, acceleration));
        const bool gravity_valid = std::isfinite(length) && length > 0.8f && length < 1.2f;
        Vec gravity{};
        if (gravity_valid)
            for (unsigned i = 0; i < 3; ++i) gravity[i] = acceleration[i] / length;
        if (!m_sample.ready)
        {
            m_sample.gyro = {};
            if (!gravity_valid) return;
            // No rotation can be integrated before a usable gyro bias exists.
            // Anchor to the current gravity, not the first pre-calibration packet.
            // Choose zero heading explicitly: shortest-arc gravity alignment is
            // ambiguous near face-down and can reverse the horizontal reference.
            const float pitch = std::atan2(gravity[2], -gravity[1]) * 0.5f;
            const float roll = std::atan2(-gravity[0],
                std::hypot(gravity[1], gravity[2])) * 0.5f;
            const float cp = std::cos(pitch), sp = std::sin(pitch);
            const float cr = std::cos(roll), sr = std::sin(roll);
            m_q = {cr*cp, cr*sp, sr*sp, sr*cp};
            Normalize();
            m_sample.ready = gyro_ready;
            UpdateMatrix();
            return;
        }
        if (!gyro_ready)
        {
            // Preserve an established reference across temporary sensor resets.
            m_sample.gyro = {};
            return;
        }
        // A transport gap is not evidence of continued rotation.
        if (!std::isfinite(dt) || dt <= 0 || dt > 0.2f) return;
        Vec corrected{};
        for (unsigned i = 0; i < 3; ++i)
        {
            m_angle[i] += static_cast<double>(m_sample.gyro[i]) * dt;
            m_sample.angle[i] = static_cast<float>(m_angle[i]);
            corrected[i] = m_sample.gyro[i] * Tau;
        }
        if (gravity_valid)
        {
            const Vec predicted{-m_sample.dir[0][1], -m_sample.dir[1][1], -m_sample.dir[2][1]};
            const Vec error = Cross(gravity, predicted);
            for (unsigned i = 0; i < 3; ++i) corrected[i] += 0.5f * error[i];
        }
        const float speed = std::sqrt(Dot(corrected, corrected));
        if (speed > 0)
        {
            const float half_angle = speed * dt * 0.5f;
            const float factor = std::sin(half_angle) / speed;
            const std::array<float, 4> d{std::cos(half_angle), corrected[0]*factor,
                corrected[1]*factor, corrected[2]*factor};
            const auto q = m_q;
            m_q = {q[0]*d[0]-q[1]*d[1]-q[2]*d[2]-q[3]*d[3],
                q[0]*d[1]+q[1]*d[0]+q[2]*d[3]-q[3]*d[2],
                q[0]*d[2]-q[1]*d[3]+q[2]*d[0]+q[3]*d[1],
                q[0]*d[3]+q[1]*d[2]-q[2]*d[1]+q[3]*d[0]};
            Normalize();
        }
        UpdateMatrix();
    }

    const Sample& GetSample() const { return m_sample; }

private:
    static constexpr float Tau = 6.283185307179586f;
    static float Dot(const Vec& a, const Vec& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
    static Vec Cross(const Vec& a, const Vec& b)
    {
        return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
    }
    void Normalize()
    {
        float length = 0;
        for (float v : m_q) length += v*v;
        length = std::sqrt(length);
        for (float& v : m_q) v /= length;
    }
    void UpdateMatrix()
    {
        const auto [w,x,y,z] = m_q;
        // Columns: each remote axis expressed in the world frame, not rows.
        m_sample.dir = {{{1-2*(y*y+z*z), 2*(x*y+w*z), 2*(x*z-w*y)},
            {2*(x*y-w*z), 1-2*(x*x+z*z), 2*(y*z+w*x)},
            {2*(x*z+w*y), 2*(y*z-w*x), 1-2*(x*x+y*y)}}};
    }
    Sample m_sample;
    std::array<double, 3> m_angle{};
    std::array<float, 4> m_q{1,0,0,0};
};
