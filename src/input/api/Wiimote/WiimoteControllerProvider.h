#pragma once
#include "input/api/Wiimote/WiimoteSpeakerProtocol.h"

#include "input/motion/MotionHandler.h"
#include "input/api/Wiimote/WiimoteDevice.h"
#include "input/api/Wiimote/WiimoteMessages.h"
#include "input/api/Wiimote/WiimoteLifecycle.h"
#include "input/api/Wiimote/WiimoteKpadMotion.h"
#include "input/api/Wiimote/WiimotePointerFilter.h"
#include "input/api/Wiimote/WiimoteStateCache.h"
#include "input/api/Wiimote/WiimoteOutputGate.h"
#include "input/api/Wiimote/WiimoteOutputSchedule.h"
#include "input/api/Wiimote/WiimoteProbeThrottle.h"
#include "input/api/Wiimote/WiimoteSpeakerCompatibility.h"
#include "input/api/Wiimote/WiimoteReportRecovery.h"

#include "input/api/ControllerProvider.h"
#include "input/api/ControllerState.h"

#include <list>
#include <span>
#include <variant>
#include <boost/ptr_container/ptr_vector.hpp>

#ifndef HAS_WIIMOTE
#define HAS_WIIMOTE 1
#endif

#define WIIMOTE_DEBUG 1

class UsbBluetoothHost;
class WiimoteControllerProvider : public ControllerProviderBase
{
	friend class WiimoteController;
public:
	constexpr static uint32 kDefaultPacketDelay = 25;

	WiimoteControllerProvider();
	~WiimoteControllerProvider();

	inline static InputAPI::Type kAPIType = InputAPI::Wiimote;
	InputAPI::Type api() const override { return kAPIType; }

	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	bool is_connected(size_t index);
	bool is_registered_device(size_t index);
	void set_rumble(size_t index, bool state);
	void request_status(size_t index);
	void set_led(size_t index, size_t player_index);
	void request_discovery();
	void set_usb_bluetooth(bool enabled);
	bool usb_bluetooth_enabled() const;
	bool set_usb_bluetooth_adapter(std::string id);
	std::string usb_bluetooth_status() const;
	void shutdown_usb_bluetooth();
	// true once the worker threads run (a profile uses the Wiimote API) or the dedicated USB adapter is enabled
	bool is_active() const { return m_threadsRunning.load(std::memory_order_relaxed) || usb_bluetooth_enabled(); }
	bool control_speaker(size_t index, uint32 command, uint8 volume,bool compatibility=false);
	void set_speaker_volume(size_t index, uint8 volume);
	bool can_send_speaker(size_t index, bool checkQueue = true);
	bool send_speaker(size_t index, std::span<const uint8> audio);

	uint32 get_packet_delay(size_t index);
	void set_packet_delay(size_t index, uint32 delay);

	struct WiimoteState
	{
		uint16 buttons = 0;
		uint8 flags = 0;
		uint8 battery_level = 0;
		bool battery_known = false;
		uint8 report_mode=0;

		glm::vec3 m_acceleration{}, m_prev_acceleration{};
		float m_roll = 0;

		std::chrono::high_resolution_clock::time_point m_last_motion_timestamp{};
		MotionSample motion_sample{};
		WiiUMotionHandler motion_handler{};
		WiimoteKpadMotion kpad_motion{};

		bool m_calibrated = false;
		Calibration m_calib_acceleration{};

		struct IRCamera
		{
			IRMode mode = kIRDisabled;
			std::array<IRDot, 4> dots{}, prev_dots{};

			glm::vec2 position{}, m_prev_position{};
			PositionVisibility m_positionVisibility;
			glm::vec2 middle {};
			float distance = 0;
			std::pair<sint32, sint32> indices{ 0,1 };
			uint32 missing_frames = 0;
			WiimotePointerFilter pointer_filter{};
			std::chrono::steady_clock::time_point pointer_timestamp{};
		}ir_camera{};

		std::optional<MotionPlusData> m_motion_plus;
		WiimoteLifecycle::Clock::time_point last_report{};
		WiimoteLifecycle::Clock::time_point next_passthrough_probe{};
		WiimoteLifecycle::Clock::time_point mpls_settle_until{};
		WiimoteLifecycle::Clock::time_point next_mpls_calibration_read{};
		bool m_motion_plus_active = false;
		std::variant<std::monostate, NunchuckData, ClassicData> m_extension{};
	};
	WiimoteState get_state(size_t index);
	std::optional<sint32> speaker_control_result(size_t index);
	using StateSnapshot = std::shared_ptr<const WiimoteState>;
	StateSnapshot get_state_snapshot(size_t index);
	

private:
#if defined(_WIN32) && defined(HAS_LIBUSB)
	std::unique_ptr<UsbBluetoothHost> m_usbBluetooth;
#endif
	std::atomic_bool m_threadsRunning{false};
	std::thread m_reader_thread, m_writer_thread;
	std::shared_mutex m_device_mutex;
	WiimoteOutputGate m_outputGate;
	std::mutex m_registration_mutex;
	WiimoteProbeThrottle<WiimoteDevice> m_probeRetry;
	WiimoteStateCache<WiimoteState,kDefaultPacketDelay> m_state_cache;

	std::thread m_connectionThread;
	std::mutex m_connectionMutex;
    std::condition_variable m_connectionCond;
	std::vector<WiimoteDevicePtr> m_connectedDevices;
	std::mutex m_connectedDeviceMutex;
	struct Wiimote
	{
		Wiimote(WiimoteDevicePtr device)
			: device(std::move(device)) {}

		WiimoteDevicePtr device;
		std::atomic_bool rumble = false;
		std::atomic_bool io_failed = false;
		std::atomic_bool speaker_requested = false, speaker_muted = true;
		std::atomic_bool speaker_playing = false;
		std::mutex speaker_control_mutex;
		WiimoteSpeakerProtocol::Operation speaker_control;
		WiimoteSpeakerProtocol::StatusRefresh status_refresh;
		bool memory_ack_pending=false;
		std::chrono::steady_clock::time_point memory_ack_deadline{};
		WiimoteOutputSchedule::AudioClock speaker_clock;
		std::atomic_bool speaker_compatibility=false;
		WiimoteSpeaker::CompatibilityStream speaker_converter;
		size_t speaker_last_length = 0;
		WiimoteLifecycle::Clock::time_point last_status_request{};

		std::shared_mutex mutex;
		WiimoteState state{};

		std::chrono::high_resolution_clock::time_point data_ts{};
	};
	boost::ptr_vector<Wiimote> m_wiimotes;

	struct OutputPacket
	{
		size_t first; std::vector<uint8> second;
		uint32 controlGeneration{}; unsigned controlStep{};
		bool restoreReport=false;
		OutputPacket(size_t device,std::vector<uint8> bytes,uint32 generation=0,unsigned step=0,bool recovery=false)
			: first(device),second(std::move(bytes)),controlGeneration(generation),controlStep(step),restoreReport(recovery) {}
	};
	std::list<OutputPacket> m_write_queue;
	std::mutex m_writer_mutex;
	std::condition_variable m_writer_cond;

	void StartThreads();
	void StopThreads();
	void FindDevices();
	void RefreshControllers();

	void reader_thread();
	void writer_thread();
	void publish_speaker_control(size_t index, Wiimote& remote);
	void connectionThread();

	void calibrate(size_t index);
	IRMode set_ir_camera(size_t index, WiimoteState& state);

	void send_packet(size_t index, std::vector<uint8> data,bool restoreReport=false);
	void send_read_packet(size_t index, MemoryType type, RegisterAddress address, uint16 size);
	void send_write_packet(size_t index, MemoryType type, RegisterAddress address, const std::vector<uint8>& data);

	void parse_acceleration(WiimoteState& wiimote_state, const uint8*& data);

	void rotate_ir(WiimoteState& wiimote_state);
	void calculate_ir_position(WiimoteState& wiimote_state);
	int parse_ir(WiimoteState& wiimote_state, const uint8* data);

	void request_extension(size_t index);
	void detect_motion_plus(size_t index);
	void set_motion_plus(size_t index, bool state);
	void set_motion_plus(size_t index, bool state, const WiimoteState& state_override);

	bool has_extension_data(const WiimoteState& state) const;
	void parse_motion_plus_data(MotionPlusData& motion_plus, const uint8* data);
	void parse_extension_data(WiimoteState& wiimote_state, const uint8* data);
	void update_motion_sample(WiimoteState& wiimote_state);

	void update_report_type(size_t index, WiimoteState& state, bool force=false);
};



