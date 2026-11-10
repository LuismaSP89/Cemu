#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// Guest pose resets must not discard a real remote's calibrated gyro bias.
struct MplsProcessing
{
    using Vec = std::array<float, 3>;
    using Matrix = std::array<Vec, 3>;
    static constexpr Matrix Identity{{{1,0,0},{0,1,0},{0,0,1}}};
    Vec last_angle{}, angle_offset{};
    Matrix last_dir = Identity, transform = Identity;
    bool calibrating = false;
    float stable_seconds = 0;
    bool dir_revise = false, acc_revise = true, dpd_revise = true;
    float dir_weight = 0.5f, acc_weight = 0.03f, acc_range = 0.4f, dpd_weight = 0.05f;
    float dir_level = 0, acc_level = 0, dpd_level = 0;
    Matrix revise_base = Identity;

    void SetAngle(const Vec& target)
    {
        for (unsigned i = 0; i < 3; ++i)
            if (std::isfinite(target[i])) angle_offset[i] = target[i] - last_angle[i];
    }
    void SetDirection(const Matrix& target)
    {
        for (const auto& column : target)
            for (float v : column) if (!std::isfinite(v)) return;
        transform = {};
        for (unsigned c = 0; c < 3; ++c)
            for (unsigned r = 0; r < 3; ++r)
                for (unsigned k = 0; k < 3; ++k)
                    transform[c][r] += target[k][r] * last_dir[k][c];
    }
    void Apply(Vec& angle, Matrix& dir)
    {
        last_angle = angle;
        last_dir = dir;
        for (unsigned i = 0; i < 3; ++i) angle[i] += angle_offset[i];
        dir = {};
        for (unsigned c = 0; c < 3; ++c)
            for (unsigned r = 0; r < 3; ++r)
                for (unsigned k = 0; k < 3; ++k)
                    dir[c][r] += transform[k][r] * last_dir[c][k];
    }
    void Correct(Matrix& dir, const Vec& acceleration, float dt,
        bool dpd_valid, const std::array<float, 2>& position, const std::array<float, 2>& horizon)
    {
        dir_level = acc_level = dpd_level = 0;
        if (!std::isfinite(dt) || dt <= 0 || dt > 0.2f) return;
        // Guest resets change the reference frame. Apply corrections in that frame,
        // rather than leaving a fixed tilted transform over the native gravity fusion.
        // Keep the requested base/weight, but do not invent an all-axis attraction
        // to it. That pins legitimate poses when games leave DirRevise enabled.
        // Its hardware correction curve remains unimplemented; sensor-backed
        // acceleration/DPD correction below must not compete with a guessed pose.
        const float length = std::sqrt(Dot(acceleration, acceleration));
        if (acc_revise && std::isfinite(length) && length > 0.01f && std::abs(length - 1) < acc_range)
        {
            Vec world_gravity{};
            for (unsigned r = 0; r < 3; ++r)
                for (unsigned c = 0; c < 3; ++c)
                    world_gravity[r] += dir[c][r] * acceleration[c] / length;
            const Vec down{0,-1,0};
            auto axis = Cross(world_gravity, down);
            float sine = std::sqrt(Dot(axis, axis));
            const float cosine = std::clamp(Dot(world_gravity, down), -1.0f, 1.0f);
            acc_level = Weight(acc_weight, dt);
            if (sine < 0.00001f && cosine < 0) { axis = {1,0,0}; sine = 1; }
            if (sine > 0.00001f)
            {
                for (float& v : axis) v /= sine;
                dir = Multiply(Rotation(axis, std::acos(cosine) * acc_level), dir);
            }
        }
        const float horizon_length = std::hypot(horizon[0], horizon[1]);
        if (dpd_revise && dpd_valid && std::isfinite(position[0]) &&
            std::isfinite(horizon_length) && horizon_length > 0.5f)
        {
            // Align the observed sensor-bar axis, not an Euler reconstruction.
            // asin(pitch) folds poses past vertical and can reverse left/right.
            // Wii Remote camera horizontal field of view is approximately 42 degrees.
            const float yaw = -std::atan(position[0] * 0.38386404f);
            Vec observed_right{};
            // Undo the guest pointer-plane roll to recover the camera's bar axis.
            for (unsigned r = 0; r < 3; ++r)
                observed_right[r] = (dir[0][r] * horizon[0] - dir[1][r] * horizon[1]) / horizon_length;
            const Vec target_right{std::cos(yaw),0,-std::sin(yaw)};
            dpd_level = Weight(dpd_weight, dt);
            dir = Multiply(Align(observed_right, target_right, dpd_level), dir);
        }
        if (dir_level > 0 || acc_level > 0 || dpd_level > 0)
        {
            Orthonormalize(dir);
            transform = Multiply(dir, Transpose(last_dir));
            Orthonormalize(transform);
        }
    }
    void SetReviseBase(const Matrix& base)
    {
        for (const auto& column : base)
            for (float v : column) if (!std::isfinite(v)) return;
        revise_base = base;
    }
    void StartCalibration() { calibrating = true; stable_seconds = 0; }
    void UpdateCalibration(const Vec& turns_per_second, bool zero_ready, float dt)
    {
        if (!calibrating) return;
        float speed2 = 0;
        for (float v : turns_per_second) speed2 += v * v;
        // SDK's final stationary threshold is 5.4 degrees/s for at least 550ms.
        if (!zero_ready || !std::isfinite(speed2) || speed2 > 0.015f * 0.015f)
            stable_seconds = 0;
        else
            stable_seconds += std::clamp(dt, 0.0f, 0.05f);
    }
    float CalibrationProgress() const
    {
        return calibrating ? std::max(0.0f, 1.0f - stable_seconds / 0.55f) : -1.0f;
    }

private:
    static float Dot(const Vec& a, const Vec& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
    static Vec Cross(const Vec& a, const Vec& b)
    { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
    static float Weight(float value, float dt)
    { return 1 - std::pow(1 - std::clamp(value, 0.0f, 1.0f), dt * 60); }
    static void Orthonormalize(Matrix& m)
    {
        const float length=std::sqrt(Dot(m[0],m[0]));
        if (!std::isfinite(length) || length < 0.0001f) { m=Identity; return; }
        for (float& v:m[0]) v/=length;
        m[2]=Cross(m[0],m[1]);
        const float zlength=std::sqrt(Dot(m[2],m[2]));
        if (!std::isfinite(zlength) || zlength < 0.0001f) { m=Identity; return; }
        for (float& v:m[2]) v/=zlength;
        m[1]=Cross(m[2],m[0]);
    }
    static Matrix Transpose(const Matrix& a)
    {
        Matrix out{};
        for (unsigned c=0;c<3;++c) for (unsigned r=0;r<3;++r) out[c][r]=a[r][c];
        return out;
    }
    static Matrix Multiply(const Matrix& a, const Matrix& b)
    {
        Matrix out{};
        for (unsigned c=0;c<3;++c) for (unsigned r=0;r<3;++r)
            for (unsigned k=0;k<3;++k) out[c][r]+=a[k][r]*b[c][k];
        return out;
    }
    static Matrix Rotation(const Vec& a, float angle)
    {
        const float c=std::cos(angle), s=std::sin(angle), t=1-c;
        const auto [x,y,z]=a;
        return {{{t*x*x+c,t*x*y+s*z,t*x*z-s*y},
                 {t*x*y-s*z,t*y*y+c,t*y*z+s*x},
                 {t*x*z+s*y,t*y*z-s*x,t*z*z+c}}};
    }
    static Matrix Align(const Vec& from, const Vec& to, float weight)
    {
        if (weight <= 0) return Identity;
        const float cosine=std::clamp(Dot(from,to),-1.0f,1.0f);
        Vec axis=Cross(from,to);
        float length=std::sqrt(Dot(axis,axis));
        if (length < 0.00001f)
        {
            if (cosine >= 0) return Identity;
            axis=Cross(from,std::abs(from[0]) < 0.8f ? Vec{1,0,0} : Vec{0,1,0});
            length=std::sqrt(Dot(axis,axis));
        }
        for(float& v:axis) v/=length;
        return Rotation(axis,std::acos(cosine)*weight);
    }
};
