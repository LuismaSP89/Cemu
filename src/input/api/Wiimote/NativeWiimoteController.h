#pragma once

#include "input/api/Controller.h"
#include "input/api/Wiimote/WiimoteControllerProvider.h"

// todo: find better name because of emulated nameclash
class NativeWiimoteController : public Controller<WiimoteControllerProvider>
{
public:
	NativeWiimoteController(size_t index);

	enum Extension
	{
		None,
		Nunchuck,
		Classic,
		MotionPlus,
	};
	
	std::string_view api_name() const override
	{
		static_assert(to_string(InputAPI::Wiimote) == "Wiimote");
		return to_string(InputAPI::Wiimote);
	}
	InputAPI::Type api() const override { return InputAPI::Wiimote; }

	void save(pugi::xml_node& node) override;
	void load(const pugi::xml_node& node) override;

	bool connect() override;
	bool is_connected() override;

	void set_player_index(size_t player_index);

	Extension get_extension() const;
	bool is_mpls_attached() const;

	ControllerState raw_state() override;

	bool has_position() override;
	glm::vec2 get_position() override;
	glm::vec2 get_prev_position() override;
	PositionVisibility GetPositionVisibility() override;
	
	bool has_motion() override { return true; }
	bool has_rumble() override { return true; }

	bool has_battery() override { return true; }
	bool has_low_battery() override;
	std::optional<uint8> battery_percent() const;
	bool control_speaker(uint32 command, uint8 volume) { return m_provider->control_speaker(m_index, command, volume,get_speaker_compatibility()); }
	std::optional<sint32> speaker_control_result() { return m_provider->speaker_control_result(m_index); }
	bool can_send_speaker(bool checkQueue = true) { return m_provider->can_send_speaker(m_index, checkQueue); }
	bool send_speaker(std::span<const uint8> audio) { return m_provider->send_speaker(m_index, audio); }
	uint8 get_speaker_volume() const { return m_speaker_volume.load(); }
	void set_speaker_volume(uint8 volume)
	{
		volume = std::min<uint8>(127, volume);
		if (m_speaker_volume.exchange(volume) != volume) m_provider->set_speaker_volume(m_index, volume);
	}
	bool get_speaker_compatibility() const { return m_speaker_compatibility.load(); }
	void set_speaker_compatibility(bool enabled) { m_speaker_compatibility.store(enabled); }
	bool usb_bluetooth_enabled() const { return m_provider->usb_bluetooth_enabled(); }
	void set_usb_bluetooth(bool enabled) { m_provider->set_usb_bluetooth(enabled); }
	std::string usb_bluetooth_status() const { return m_provider->usb_bluetooth_status(); }
	
	void start_rumble() override;
	void stop_rumble() override;

	MotionSample get_motion_sample() override;
	WiimoteKpadMotion::Sample get_kpad_motion() const;
	MotionSample get_nunchuck_motion_sample() const;
	glm::vec2 get_nunchuck_axis() const;
	std::optional<MotionPlusData> get_motion_plus_data() const;

	std::string get_button_name(uint64 button) const override;

	uint32 get_packet_delay();
	void set_packet_delay(uint32 delay);

private:
	size_t m_index;
	std::atomic<uint8> m_speaker_volume{64};
	std::atomic_bool m_speaker_compatibility{false};
	std::atomic<size_t> m_player_index{0};
	uint32 m_packet_delay = WiimoteControllerProvider::kDefaultPacketDelay;
};

