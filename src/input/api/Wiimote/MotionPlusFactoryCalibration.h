#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// MotionPlus register 0x20: fast/slow 16-byte blocks, with a split CRC32.
// Protocol reference: Dolphin Core/HW/WiimoteEmu/MotionPlus.{h,cpp}.
struct MotionPlusFactoryCalibration
{
	std::array<std::uint8_t, 32> bytes{};
	std::array<float, 3> slowBias{}; // roll, pitch, yaw; 14-bit report units

	bool Receive(std::uint16_t offset, const std::uint8_t* data, std::size_t size)
	{
		if (size != 16 || (offset != 0x20 && offset != 0x30))
			return false;
		const std::size_t block = (offset - 0x20) / 16;
		for (std::size_t i = 0; i < size; ++i)
			bytes[block * 16 + i] = data[i];
		m_received |= 1u << block;
		if (m_received != 3)
			return false;

		std::uint32_t crc = 0xffffffffu;
		for (std::size_t i = 0; i < bytes.size(); ++i)
		{
			if (i % 16 >= 14)
				continue;
			crc ^= bytes[i];
			for (int bit = 0; bit < 8; ++bit)
				crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
		if (~crc != ((static_cast<std::uint32_t>(Read16(14)) << 16) | Read16(30)))
			return false;
		// Reject invalid blocks even if a broken device supplied a matching CRC.
		for (std::size_t blockOffset : {0u, 16u})
		{
			if (bytes[blockOffset + 12] == 0)
				return false;
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				const auto zero = Read16(blockOffset + axis * 2);
				if (zero == 0 || zero == 0xffff || zero == Read16(blockOffset + 6 + axis * 2))
					return false;
			}
		}
		slowBias = {Read16(18) / 4.0f, Read16(20) / 4.0f, Read16(16) / 4.0f};
		return true;
	}

private:
	std::uint32_t m_received = 0;
	std::uint16_t Read16(std::size_t offset) const
	{
		return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
	}
};
