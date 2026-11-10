#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

struct MotionPlusZeroCalibration
{
	std::array<float, 3> bias{};
	bool ready = false;

	void ResetPendingSamples()
	{
		m_samples = 0;
		m_sum = {};
	}

	bool SeedFactoryBias(const std::array<float, 3>& factoryBias)
	{
		if (ready)
			return false;
		for (float value : factoryBias)
			if (!std::isfinite(value) || value <= 0.0f || value >= 16383.0f)
				return false;
		bias = factoryBias;
		ready = true;
		return true;
	}

	std::array<float, 3> Update(const std::array<float, 3>& raw, bool allSlow)
	{
		if (!m_restCalibrated)
		{
			if (!allSlow)
				ResetPendingSamples();
			else
			{
				// A factory bias allows live input before the first quiet window.
				// Keep the original quiet-window checks for temperature refinement.
				for (std::size_t axis = 0; axis < 3 && m_samples != 0; ++axis)
				{
					if (std::max(m_max[axis], raw[axis]) - std::min(m_min[axis], raw[axis]) > 64.0f)
						ResetPendingSamples();
				}
				if (m_samples == 0)
					m_min = m_max = raw;
				for (std::size_t axis = 0; axis < 3; ++axis)
				{
					m_min[axis] = std::min(m_min[axis], raw[axis]);
					m_max[axis] = std::max(m_max[axis], raw[axis]);
					m_sum[axis] += raw[axis];
				}
				if (++m_samples >= 45)
				{
					for (std::size_t axis = 0; axis < 3; ++axis)
						bias[axis] = m_sum[axis] / static_cast<float>(m_samples);
					ready = m_restCalibrated = true;
				}
			}
			if (!ready)
				return {};
		}

		std::array<float, 3> delta{};
		bool nearRest = allSlow;
		for (std::size_t axis = 0; axis < 3; ++axis)
		{
			delta[axis] = raw[axis] - bias[axis];
			nearRest &= std::abs(delta[axis]) <= 12.0f;
		}
		if (nearRest)
		{
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				bias[axis] += delta[axis] * 0.005f;
				delta[axis] = raw[axis] - bias[axis];
				if (std::abs(delta[axis]) <= 1.0f)
					delta[axis] = 0.0f;
			}
		}
		return delta;
	}

private:
	std::array<float, 3> m_sum{}, m_min{}, m_max{};
	std::uint32_t m_samples = 0;
	bool m_restCalibrated = false;
};
