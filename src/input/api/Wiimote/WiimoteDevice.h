#pragma once

class WiimoteDevice
{
	friend class WiimoteInfo;
public:
	virtual ~WiimoteDevice() = default;

	virtual bool write_data(const std::vector<uint8>& data) = 0;
	virtual bool write_audio_data(const std::vector<uint8>& data,bool compatibility) { return write_data(data); }
	virtual std::optional<std::vector<uint8_t>> read_data() = 0;

	virtual bool operator==(const WiimoteDevice& o) const = 0;

	// Transports that know a remote's identity (bonded USB remotes) ask for a fixed provider slot, so
	// "Controller N" in profiles always means the same physical remote regardless of connection order.
	virtual std::optional<size_t> preferred_slot() const { return std::nullopt; }
};

using WiimoteDevicePtr = std::shared_ptr<WiimoteDevice>;
