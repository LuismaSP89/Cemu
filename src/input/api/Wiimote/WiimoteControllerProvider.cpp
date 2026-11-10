#include "input/api/Wiimote/WiimoteControllerProvider.h"
#include "input/api/Wiimote/NativeWiimoteController.h"
#include "input/api/Wiimote/WiimoteMessages.h"
#include "input/api/Wiimote/WiimoteSpeaker.h"
#include "input/api/Wiimote/WiimoteOutputSchedule.h"
#include "input/api/Wiimote/WiimoteOutputWait.h"
#include "input/api/Wiimote/WiimoteSpeakerTrace.h"
#include "config/CemuConfig.h"
#if defined(_WIN32) && defined(HAS_LIBUSB)
#include "input/api/Wiimote/usb/UsbBluetoothHost.h"
#endif

#ifdef HAS_HIDAPI
#include "input/api/Wiimote/hidapi/HidapiWiimote.h"
#endif
#ifdef HAS_BLUEZ
#include "input/api/Wiimote/l2cap/L2CapWiimote.h"
#endif

#include <numbers>
#include <queue>
#include <algorithm>
#include <tuple>

WiimoteControllerProvider::WiimoteControllerProvider()
{
#if defined(_WIN32) && defined(HAS_LIBUSB)
	m_usbBluetooth = std::make_unique<UsbBluetoothHost>();
	if (GetConfig().wiimote_usb_bluetooth.GetValue())
	{
		// the dedicated adapter must answer SYNC presses even before any profile uses the Wiimote API
		m_usbBluetooth->SetEnabled(true);
		StartThreads();
	}
#endif
}

void WiimoteControllerProvider::StartThreads()
{
	if (!m_threadsRunning.exchange(true))
	{
		FindDevices(); // get controller list immediately so it's available when StartThreads() returns (as it is called by get_controllers initially)
		m_reader_thread = std::thread(&WiimoteControllerProvider::reader_thread, this);
		m_writer_thread = std::thread(&WiimoteControllerProvider::writer_thread, this);
		m_connectionThread = std::thread(&WiimoteControllerProvider::connectionThread, this);
	}
}

WiimoteControllerProvider::~WiimoteControllerProvider()
{
#if defined(_WIN32) && defined(HAS_LIBUSB)
	if (m_usbBluetooth)
		m_usbBluetooth->SetEnabled(false);
#endif
	StopThreads();
}


void WiimoteControllerProvider::StopThreads()
{
	if (m_threadsRunning.exchange(false))
	{
		{
			std::scoped_lock lock(m_writer_mutex); 
			m_writer_cond.notify_all();
		}

		{
			std::scoped_lock lock(m_connectionMutex);
			m_connectionCond.notify_all();
		}

		if (m_writer_thread.joinable())
		{
			m_writer_thread.join();
		}

		if (m_reader_thread.joinable())
		{
			m_reader_thread.join();
		}

		if (m_connectionThread.joinable())
		{
			m_connectionThread.join();
		}
	}
}

void WiimoteControllerProvider::shutdown_usb_bluetooth()
{
#if defined(_WIN32) && defined(HAS_LIBUSB)
	m_usbBluetooth->Shutdown();
#endif
}

void WiimoteControllerProvider::set_usb_bluetooth(bool enabled)
{
#if defined(_WIN32) && defined(HAS_LIBUSB)
	m_usbBluetooth->SetEnabled(enabled);
	GetConfig().wiimote_usb_bluetooth = enabled;
	GetConfigHandle().Save();
	if (enabled)
		StartThreads();
#endif
	m_connectionCond.notify_one();
}

bool WiimoteControllerProvider::usb_bluetooth_enabled() const
{
#if defined(_WIN32) && defined(HAS_LIBUSB)
	return m_usbBluetooth->Enabled();
#else
	return false;
#endif
}

bool WiimoteControllerProvider::set_usb_bluetooth_adapter(std::string id)
{
#if defined(_WIN32) && defined(HAS_LIBUSB)
	if (!m_usbBluetooth->SetAdapterId(id)) return false;
	GetConfig().wiimote_usb_adapter = std::move(id);
	GetConfigHandle().Save();
	return true;
#else
	return false;
#endif
}

std::string WiimoteControllerProvider::usb_bluetooth_status() const
{
#if defined(_WIN32) && defined(HAS_LIBUSB)
	return m_usbBluetooth->Status();
#else
	return "USB Bluetooth requires a Windows/libusb build";
#endif
}

void WiimoteControllerProvider::request_discovery()
{
#if defined(_WIN32) && defined(HAS_LIBUSB)
	m_usbBluetooth->RequestSync();
#endif
	m_connectionCond.notify_one();
}

std::vector<std::shared_ptr<ControllerBase>> WiimoteControllerProvider::get_controllers()
{
	StartThreads();
	RefreshControllers();

	size_t count;
	{
		std::shared_lock lock(m_device_mutex);
		count = m_wiimotes.size();
	}
	std::vector<std::shared_ptr<ControllerBase>> result;
	result.reserve(count);
	for (size_t i = 0; i < count; ++i)
	{
		result.emplace_back(std::make_shared<NativeWiimoteController>(i));
	}

	return result;
}

void WiimoteControllerProvider::RefreshControllers()
{
	std::scoped_lock registration(m_registration_mutex);
	m_connectedDeviceMutex.lock();
	auto devices = m_connectedDevices;
	m_connectedDeviceMutex.unlock();

	auto lock = m_outputGate.LockInput(m_device_mutex);

	const auto inspect = [&](const WiimoteDevicePtr& device)
	{
		bool isDuplicate = false;
		ssize_t lowestReplaceableIndex = -1;
		for (size_t i = m_wiimotes.size(); i-- > 0;)
		{
			const auto& wiimoteDevice = m_wiimotes[i].device;
			if (wiimoteDevice)
			{
				if (*wiimoteDevice == *device)
				{
					isDuplicate = true;
					break;
				}
				continue;
			}

			lowestReplaceableIndex = static_cast<ssize_t>(i);
		}
		return std::pair{isDuplicate, lowestReplaceableIndex};
	};
	for (auto& device : devices)
	{
		auto [isDuplicate, lowestReplaceableIndex] = inspect(device);
		if (isDuplicate)
			continue;
		// Discovery probes can block too; never serialize game audio behind one.
		lock.unlock();
		if (!m_probeRetry.Ready(device, std::chrono::steady_clock::now()))
		{
			lock = m_outputGate.LockInput(m_device_mutex);
			continue;
		}
		const auto probeStart = std::chrono::steady_clock::now();
		const bool probed = device->write_data({kStatusRequest, 0x00});
		const auto probeEnd = std::chrono::steady_clock::now();
		if (probed) m_probeRetry.Succeeded(device);
		else m_probeRetry.Failed(device, probeEnd);
		const std::array<uint8,2> probeReport{kStatusRequest,0};
		WiimoteSpeakerTrace::Packet(WiimoteSpeakerTrace::Stage::Probe,
			reinterpret_cast<std::uintptr_t>(device.get()),probed,probeReport,
			static_cast<uint32>(std::chrono::duration_cast<std::chrono::microseconds>(probeEnd-probeStart).count()));
		lock = m_outputGate.LockInput(m_device_mutex);
		if (!probed)
			continue;
		std::tie(isDuplicate, lowestReplaceableIndex) = inspect(device);
		if (isDuplicate)
			continue;
		if (lowestReplaceableIndex != -1)
		{
			m_state_cache.Clear(lowestReplaceableIndex);
			m_wiimotes.replace(lowestReplaceableIndex, std::make_unique<Wiimote>(device));
		}
		else
			m_wiimotes.push_back(std::make_unique<Wiimote>(device));
	}
}

bool WiimoteControllerProvider::is_connected(size_t index)
{
	const auto state=get_state_snapshot(index);
	return WiimoteLifecycle::IsLive(state->last_report,WiimoteLifecycle::Clock::now());
}

bool WiimoteControllerProvider::is_registered_device(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	return index < m_wiimotes.size();
}

void WiimoteControllerProvider::set_rumble(size_t index, bool state)
{
	std::shared_lock lock(m_device_mutex);
	if (index >= m_wiimotes.size())
		return;
	
	if (m_wiimotes[index].rumble.exchange(state) == state)
		return;
	lock.unlock();

	// Rumble has its own report. A status reply can trigger unrelated register I/O.
	send_packet(index, { kRumble, 0x00 });
}

void WiimoteControllerProvider::request_status(size_t index)
{
	send_packet(index, {kStatusRequest, 0x00});
}

bool WiimoteControllerProvider::control_speaker(size_t index, uint32 command, uint8 volume,bool compatibility)
{
	if (!is_connected(index) || command > 5) return false;
	std::shared_lock devices(m_device_mutex);
	if (index >= m_wiimotes.size() || !m_wiimotes[index].device) return false;
	std::scoped_lock queue(m_writer_mutex);
	auto& remote = m_wiimotes[index];
	std::scoped_lock control(remote.speaker_control_mutex);
	if (!remote.speaker_control.Begin(command,volume,{remote.speaker_requested,remote.speaker_muted,remote.speaker_playing},std::chrono::steady_clock::now(),compatibility)) return false;
	if(command==1 || command==5)
	{
		remote.speaker_compatibility=compatibility;
		remote.speaker_clock.SetCompatibility(compatibility);
		cemuLog_log(LogType::Force,"Wiimote speaker[{}]: speaker compatibility={} physical_rate={}Hz guest_rate=6000Hz",index,compatibility,compatibility?3000:6000);
	}
	if(command==0 || command==1 || command==4 || command==5) remote.speaker_converter.Reset();
	// Reset/off discards the old encoded stream before resetting the hardware decoder.
	m_write_queue.remove_if([&](const auto& packet) { return packet.first==index &&
		(packet.controlGeneration || ((command==0 || command==1 || command==5) &&
			(packet.second[0]==kSpeakerData || WiimoteSpeaker::IsVolume(packet.second)))); });
	const auto insertion=std::find_if(m_write_queue.begin(),m_write_queue.end(),[&](const auto& p){return p.first==index;});
	unsigned step=0;
	for (const auto& operation : remote.speaker_control.Steps())
		m_write_queue.emplace(insertion,index,operation.packet,remote.speaker_control.generation,step++);
	m_state_cache.SetSpeaker(index,remote.speaker_requested,remote.speaker_muted,remote.speaker_playing,true);
	publish_speaker_control(index,remote);
	m_writer_cond.notify_one();
	return true;
}

void WiimoteControllerProvider::set_speaker_volume(size_t index, uint8 volume)
{
	if (!is_connected(index)) return;
	std::shared_lock devices(m_device_mutex);
	if (index >= m_wiimotes.size() || !m_wiimotes[index].device) return;
	std::scoped_lock queue(m_writer_mutex);
	auto& remote = m_wiimotes[index];
	std::scoped_lock control(remote.speaker_control_mutex);
	const bool pending = remote.speaker_control.Pending();
	const auto command = remote.speaker_control.Command();
	if (pending ? (command == 0 || (!remote.speaker_requested && command != 1 && command != 5)) : !remote.speaker_requested)
		return;
	WiimoteSpeaker::QueueVolume(m_write_queue, index, volume);
	m_writer_cond.notify_one();
}

void WiimoteControllerProvider::publish_speaker_control(size_t index, Wiimote& remote)
{
	if (const auto result=remote.speaker_control.TakeCompletion())
	{
		const auto state=*result==0 ? remote.speaker_control.FinalState() : WiimoteSpeakerProtocol::State{};
		remote.speaker_requested=state.enabled;remote.speaker_muted=state.muted;remote.speaker_playing=state.playing;
		m_state_cache.SetSpeaker(index,state.enabled,state.muted,state.playing,false);
		cemuLog_log(LogType::Force,"Wiimote speaker control[{}]: command={} hardware_result={} enabled={} playing={} muted={}",index,remote.speaker_control.Command(),*result,state.enabled,state.playing,state.muted);
	}
}

std::optional<sint32> WiimoteControllerProvider::speaker_control_result(size_t index)
{
	std::shared_lock devices(m_device_mutex);
	if(index>=m_wiimotes.size() || !m_wiimotes[index].device) return -1;
	auto& remote=m_wiimotes[index];
	std::scoped_lock control(remote.speaker_control_mutex);
	remote.speaker_control.Poll(std::chrono::steady_clock::now());
	publish_speaker_control(index,remote);
	return remote.speaker_control.Result();
}

bool WiimoteControllerProvider::can_send_speaker(size_t index, bool checkQueue)
{
	const auto entry=m_state_cache.Read(index);
	if (!entry.sample || !WiimoteLifecycle::IsLive(entry.sample->last_report,WiimoteLifecycle::Clock::now())) return false;
	if (!entry.speakerRequested || !entry.speakerPlaying || entry.speakerPending || !(entry.sample->flags & kSpeakerEnabled)) return false;
	if (!checkQueue) return true;
	std::scoped_lock queue_lock(m_writer_mutex);
	if (std::any_of(m_write_queue.begin(),m_write_queue.end(),[index](const auto& item) {return item.first==index && !WiimoteOutputSchedule::IsAudio(item.second[0]) && !WiimoteOutputSchedule::AudioMayPass(item.second[0]);})) return false;
	return std::count_if(m_write_queue.begin(), m_write_queue.end(), [index](const auto& item) { return item.first == index && item.second[0] == kSpeakerData; }) < 6;
}

bool WiimoteControllerProvider::send_speaker(size_t index, std::span<const uint8> audio)
{
	const bool accepted=!audio.empty() && audio.size()<=20 && can_send_speaker(index);
	WiimoteSpeakerTrace::Packet(WiimoteSpeakerTrace::Stage::Queued,index,accepted,audio);
	if (!accepted) return false;
	std::shared_lock devices(m_device_mutex);
	if(index>=m_wiimotes.size() || !m_wiimotes[index].device)return false;
	std::scoped_lock queue(m_writer_mutex);
	auto& remote=m_wiimotes[index];
	std::scoped_lock control(remote.speaker_control_mutex);
	if(remote.speaker_control.Pending() || !remote.speaker_playing)return false;
	const auto converted=remote.speaker_compatibility?remote.speaker_converter.Convert(audio):std::vector<uint8>(audio.begin(),audio.end());
	if(converted.empty())return true;
	bool merged=false;
	if(remote.speaker_compatibility)
	{
		// Coalesce 6 kHz guest chunks into full 3 kHz reports; partial cue tails still send.
		auto last=std::find_if(m_write_queue.rbegin(),m_write_queue.rend(),[index](const auto& item){return item.first==index;});
		if(last!=m_write_queue.rend())merged=WiimoteSpeaker::AppendPacket(last->second,converted);
		const auto now=std::chrono::steady_clock::now();
		if(last==m_write_queue.rend() && remote.speaker_clock.Deadline()<now)
			remote.speaker_clock.NotBefore(now+std::chrono::milliseconds(8));
	}
	if(!merged)m_write_queue.emplace_back(index,WiimoteSpeaker::Data(converted));
	m_writer_cond.notify_one();
	return true;
}

void WiimoteControllerProvider::set_led(size_t index, size_t player_index)
{
	uint8 mask = 0;
	mask |= 1 << (4 + (player_index % 4));
	if (player_index >= 4)
		mask |= 1 << (4 + ((player_index - 3) % 4));
	send_packet(index, {kLED, mask});
}

uint32 WiimoteControllerProvider::get_packet_delay(size_t index)
{
	return m_state_cache.Read(index).packetDelay;
}

void WiimoteControllerProvider::set_packet_delay(size_t index, uint32 delay)
{
	m_state_cache.SetDelay(index,delay);
}

WiimoteControllerProvider::WiimoteState WiimoteControllerProvider::get_state(size_t index)
{
	return *get_state_snapshot(index);
}

WiimoteControllerProvider::StateSnapshot WiimoteControllerProvider::get_state_snapshot(size_t index)
{
	const auto entry=m_state_cache.Read(index);
	if (entry.sample && WiimoteLifecycle::IsLive(entry.sample->last_report,WiimoteLifecycle::Clock::now())) return entry.sample;
	static const auto disconnected=std::make_shared<const WiimoteState>();
	return disconnected;
}

void WiimoteControllerProvider::FindDevices()
{
	std::vector<WiimoteDevicePtr> devices;
#ifdef HAS_HIDAPI
	const auto& hidDevices = HidapiWiimote::get_devices();
	std::ranges::move(hidDevices, std::back_inserter(devices));
#endif
#ifdef HAS_BLUEZ
	const auto& l2capDevices = L2CapWiimote::get_devices();
	std::ranges::move(l2capDevices, std::back_inserter(devices));
#endif
#if defined(_WIN32) && defined(HAS_LIBUSB)
	auto usbDevices = m_usbBluetooth->Devices();
	std::ranges::move(usbDevices, std::back_inserter(devices));
#endif
	std::scoped_lock lock(m_connectedDeviceMutex);
	m_connectedDevices = std::move(devices);
}

void WiimoteControllerProvider::connectionThread()
{
	SetThreadName("Wiimote-connect");
	while (m_threadsRunning.load(std::memory_order_relaxed))
	{
		std::unique_lock<std::mutex> lock(m_connectionMutex);
		if (m_connectionCond.wait_for(lock, std::chrono::seconds(2), [this]() { return !m_threadsRunning.load(std::memory_order_relaxed); }))
			return;
		lock.unlock();
		FindDevices();
	}
}


void WiimoteControllerProvider::reader_thread()
{
	const bool traceLifecycle=std::getenv("CEMU_WIIMOTE_LIFECYCLE_TRACE")!=nullptr;
	const bool traceNunchuk=std::getenv("CEMU_WIIMOTE_NUNCHUK_TRACE")!=nullptr;
	struct NunchukTrace { WiimoteLifecycle::Clock::time_point next{}; bool held=false; };
	std::unordered_map<size_t,NunchukTrace> nunchukTrace;
	SetThreadName("Wiimote-reader");
	std::chrono::steady_clock::time_point lastCheck = {};
	while (m_threadsRunning.load(std::memory_order_relaxed))
	{
		const auto now = std::chrono::steady_clock::now();
		if (now - lastCheck > std::chrono::milliseconds(500))
		{
			// check for new connected wiimotes
			RefreshControllers();
			lastCheck = std::chrono::steady_clock::now();
		}

		bool receivedAnyPacket = false;
		auto lock = m_outputGate.LockInput(m_device_mutex);
		for (size_t index = 0; index < m_wiimotes.size(); ++index)
		{
			auto& wiimote = m_wiimotes[index];
			if (!wiimote.device)
				continue;
			if (wiimote.io_failed.exchange(false))
			{
				m_state_cache.Clear(index);
				wiimote.device.reset();
				wiimote.state = {};
				wiimote.rumble = false;
				std::scoped_lock queue_lock(m_writer_mutex);
				m_write_queue.remove_if([index](const auto& packet) { return packet.first == index; });
				continue;
			}
			if (wiimote.state.last_report != WiimoteLifecycle::Clock::time_point{} &&
				!WiimoteLifecycle::IsLive(wiimote.state.last_report, now))
			{
				cemuLog_log(LogType::InputAPI, "Wiimote[{}] disconnected: input reports timed out", index);
				m_state_cache.Clear(index);
				wiimote.state = {};
				wiimote.rumble = false;
				std::scoped_lock queue_lock(m_writer_mutex);
				m_write_queue.remove_if([index](const auto& packet) { return packet.first == index; });
			}
			if (now - wiimote.last_status_request >= std::chrono::seconds(1))
			{
				request_status(index);
				wiimote.last_status_request = now;
			}

			// Keep the handle alive, not the registry lock, across host HID I/O.
			const auto readDevice = wiimote.device;
			const auto* readSlot = &wiimote;
			lock.unlock();
			const auto read_data = readDevice->read_data();
			lock = m_outputGate.LockInput(m_device_mutex);
			if (index >= m_wiimotes.size() || &m_wiimotes[index] != readSlot ||
				m_wiimotes[index].device != readDevice)
				continue;
			if (!read_data)
			{
				m_state_cache.Clear(index);
				wiimote.io_failed = true;
				continue;
			}
			if (read_data->empty())
				continue;
			if (!WiimoteLifecycle::ValidReport(read_data->front(), read_data->size()))
				continue;
			receivedAnyPacket = true;

			std::shared_lock read_lock(wiimote.mutex);
			WiimoteState new_state = wiimote.state;
			read_lock.unlock();
			if (new_state.last_report == WiimoteLifecycle::Clock::time_point{})
				cemuLog_log(LogType::InputAPI, "Wiimote[{}] connected: valid input report", index);
			new_state.last_report = now;

			bool update_report = false;
			bool force_report = false;

			const uint8* data = read_data->data();
			const auto id = (InputReportId)*data;
			++data;
			const auto log_raw_report = [&](const char* label)
			{
				static uint32 s_rawPacketLogCounter = 0;
				if ((s_rawPacketLogCounter++ % 25) != 0)
					return;

				const size_t packet_size = read_data->size();
				const auto byte_at = [&](size_t i) -> uint32
				{
					return i < packet_size ? static_cast<uint32>((*read_data)[i]) : 0u;
				};

				cemuLog_log(
					LogType::InputAPI,
					"Wiimote raw {} idx={} id={:02x} size={} bytes={:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
					label,
					static_cast<uint32>(index),
					static_cast<uint32>(id),
					static_cast<uint32>(packet_size),
					byte_at(0), byte_at(1), byte_at(2), byte_at(3), byte_at(4), byte_at(5),
					byte_at(6), byte_at(7), byte_at(8), byte_at(9), byte_at(10), byte_at(11),
					byte_at(12), byte_at(13), byte_at(14), byte_at(15), byte_at(16), byte_at(17),
					byte_at(18), byte_at(19), byte_at(20), byte_at(21));
			};
			switch (id)
			{
			case kStatus:
				{
                    cemuLog_logDebug(LogType::Force,"WiimoteControllerProvider::read_thread: kStatus");
					new_state.buttons = (*(uint16*)data) & (~0x60E0);
					data += 2;
					const auto previous_flags = new_state.flags;
					new_state.flags = *data;
					++data;
					data += 2; // skip zeroes
					new_state.battery_level = *data;
					new_state.battery_known = true;
					++data;

					if(!new_state.m_calibrated)
						calibrate(index);

					if(!new_state.m_motion_plus)
						detect_motion_plus(index);

					if (HAS_FLAG(new_state.flags, kExtensionConnected))
					{
                        cemuLog_logDebug(LogType::Force,"Extension flag is set");
						if(!new_state.m_motion_plus &&
							(new_state.m_extension.index() == 0 || !(previous_flags & kExtensionConnected)))
							request_extension(index);
					}
					else
					{
						if (new_state.m_motion_plus_active && (previous_flags & kExtensionConnected) &&
							now >= new_state.mpls_settle_until)
						{
							new_state.m_motion_plus.reset();
							new_state.m_motion_plus_active = false;
						}
						if (!new_state.m_motion_plus_active)
							new_state.m_extension = {};
					}

					update_report = wiimote.status_refresh.Received(previous_flags,new_state.flags,new_state.m_calibrated);
					force_report = update_report;
					if(traceLifecycle) cemuLog_log(LogType::Force,"Wiimote lifecycle[{}]: status flags={:02x} restore_reports={} MotionPlus={} active={} extension={}",index,new_state.flags,update_report,new_state.m_motion_plus.has_value(),new_state.m_motion_plus_active,new_state.m_extension.index());
					{
						std::scoped_lock control(wiimote.speaker_control_mutex);
						wiimote.speaker_control.Status(new_state.flags);
						publish_speaker_control(index,wiimote);
					}
				}
				break;
			case kRead:
				{
                    cemuLog_logDebug(LogType::Force,"WiimoteControllerProvider::read_thread: kRead");
					new_state.buttons = (*(uint16*)data) & (~0x60E0);
					data += 2;
					const uint8 error_flag = *data & 0xF, size = (*data >> 4) + 1;
					++data;

					if (error_flag)
					{
						if(traceLifecycle) cemuLog_log(LogType::Force,"Wiimote lifecycle[{}]: register read error={} offset={:04x}",index,error_flag,(uint32(data[0])<<8)|data[1]);

						// 7 means that wiimote is already enabled or not available
                        cemuLog_logDebug(LogType::Force,"Received error on data read {:#x}", error_flag);
						continue;
					}

					auto address = *(betype<uint16>*)data;
					data += 2;
					if (traceLifecycle && size >= 6 && (address == 0xfa || address == 0xf6 || address == 0x16))
						cemuLog_log(LogType::Force,"Wiimote lifecycle[{}]: read offset={:04x} size={} bytes={:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
							index,address,size,data[0],data[1],data[2],data[3],data[4],data[5]);
					if (address == (kRegisterCalibration & 0xFFFF))
					{
                        cemuLog_logDebug(LogType::Force,"Calibration received");

						if (size != 8) break;

						new_state.m_calib_acceleration.zero.x = (uint16)*data << 2;
						++data;
						new_state.m_calib_acceleration.zero.y = (uint16)*data << 2;
						++data;
						new_state.m_calib_acceleration.zero.z = (uint16)*data << 2;
						++data;
						// --XXYYZZ
						new_state.m_calib_acceleration.zero.x |= (*data >> 4) & 0x3; // 5|4 -> 1|0
						new_state.m_calib_acceleration.zero.y |= (*data >> 2) & 0x3; // 3|4 -> 1|0
						new_state.m_calib_acceleration.zero.z |= *data & 0x3;
						++data;

						new_state.m_calib_acceleration.gravity.x = (uint16)*data << 2;
						++data;
						new_state.m_calib_acceleration.gravity.y = (uint16)*data << 2;
						++data;
						new_state.m_calib_acceleration.gravity.z = (uint16)*data << 2;
						++data;
						new_state.m_calib_acceleration.gravity.x |= (*data >> 4) & 0x3; // 5|4 -> 1|0
						new_state.m_calib_acceleration.gravity.y |= (*data >> 2) & 0x3; // 3|4 -> 1|0
						new_state.m_calib_acceleration.gravity.z |= *data & 0x3;
						++data;

						new_state.m_calibrated = true;
					}
					else if (address == (kRegisterExtensionType & 0xFFFF))
					{
						if (size == 0xf)
						{
							cemuLog_logDebug(LogType::Force,"Extension type received but no extension connected");
							cemuLog_log(LogType::InputAPI, "Wiimote[{}] extension type read returned no extension", index);
							continue;
						}

						if (size != 6) break;
						auto be_type = *(betype<uint64>*)data;
						data += 6; // 48
						be_type >>= 16;
						be_type &= 0xFFFFFFFFFFFF;
						const auto extension_type = be_type.value();
						const auto extension_type_low32 = static_cast<uint32>(extension_type & 0xFFFFFFFFull);
						// Some real MotionPlus variants report extra mode/status bits in the
						// low flag byte while keeping the actual attachment ID intact.
						// Normalize those bits so we still recognize the underlying type.
						const bool is_mpls_id = WiimoteLifecycle::IsMotionPlusId(extension_type_low32);
						switch (extension_type)
						{
						case kExtensionNunchuck:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Nunchuck");
							new_state.m_extension = NunchuckData{};
							break;
						case kExtensionClassic:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Classic");
							new_state.m_extension = ClassicData{};
							break;
						case kExtensionClassicPro:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Classic Pro");
							new_state.m_extension = ClassicData{};
                            break;
						case kExtensionGuitar:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Guitar");
                            break;
						case kExtensionDrums:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Drums");
                            break;
						case kExtensionBalanceBoard:
                            cemuLog_logDebug(LogType::Force,"Extension Type Received: Balance Board");
                            break;
						case kExtensionPartialyInserted:
                            cemuLog_logDebug(LogType::Force,"Extension only partially inserted");
							new_state.m_extension = {};
							request_status(index);
							break;
						default:
							if (is_mpls_id)
							{
								const bool inactive = (extension_type_low32 & 0xffff0000u) == 0xa6200000u;
								cemuLog_log(LogType::InputAPI, "Wiimote[{}] MotionPlus ID={:#x} inactive={}", index, extension_type, inactive);
								if (!new_state.m_motion_plus)
									new_state.m_motion_plus.emplace();
								if (inactive)
								{
									set_motion_plus(index, true, new_state);
									new_state.mpls_settle_until = now + std::chrono::milliseconds(500);
								}
								break;
							}

							cemuLog_logDebug(LogType::Force,"Unknown extension: {:#x}", extension_type);
							cemuLog_log(LogType::InputAPI, "Wiimote[{}] detected unknown extension {:#x}", index, extension_type);
							new_state.m_extension = {};
							break;
						}

						if (new_state.m_motion_plus && !is_mpls_id && new_state.m_extension.index() != 0)
						{
							set_motion_plus(index, true, new_state);
							new_state.mpls_settle_until = now + std::chrono::milliseconds(500);
						}

						// While MotionPlus passthrough is active, extension reports are interleaved and
						// the normal extension register block is not reliable enough to use for
						// Nunchuck/Classic calibration reads. Keep the sane built-in defaults instead
						// of overwriting them with garbage calibration data.
						if (new_state.m_extension.index() != 0 && !new_state.m_motion_plus.has_value())
							send_read_packet(index, kRegisterMemory, kRegisterExtensionCalibration, 0x10);
					}
					else if (address == (kRegisterPassthroughType & 0xFFFF))
					{
						if (size != 4 || !new_state.m_motion_plus_active ||
							!new_state.m_motion_plus->extension_connected)
							break;
						const auto type = WiimoteLifecycle::PassthroughType(data);
						if (type == 1) new_state.m_extension = NunchuckData{};
						else if (type == 2) new_state.m_extension = ClassicData{};
						if (type != 0)
						{
							// Change the active bank's mode without resetting gyro calibration.
							send_write_packet(index, kRegisterMemory, kRegisterPassthroughMode,
								{static_cast<uint8>(type == 1 ? 0x05 : 0x07)});
							new_state.mpls_settle_until = now + std::chrono::milliseconds(500);
							cemuLog_log(LogType::InputAPI, "Wiimote[{}] passthrough extension attached type={}", index, type);
						}
					}
					else if ((address == 0x20 || address == 0x30) && new_state.m_motion_plus_active &&
						new_state.m_motion_plus && new_state.m_motion_plus->factory_calibration_pending)
					{
						auto& mpls = *new_state.m_motion_plus;
						if (mpls.factory_calibration.Receive(address, data, size))
						{
							mpls.factory_calibration_loaded = true;
							mpls.factory_calibration_pending = false;
							const bool seeded = mpls.zero_calibration.SeedFactoryBias(mpls.factory_calibration.slowBias);
							const auto& bias = mpls.factory_calibration.slowBias;
							cemuLog_log(LogType::InputAPI,
								"Wiimote[{}] MotionPlus factory calibration CRC valid bias=({},{},{}) seeded={}",
								index, bias[0], bias[1], bias[2], seeded);
						}
					}
					else if (address == (kRegisterExtensionCalibration & 0xFFFF))
					{
						if (size != 0x10 || new_state.m_motion_plus) break;
                        cemuLog_logDebug(LogType::Force,"Extension calibration received");
						if (std::holds_alternative<NunchuckData>(new_state.m_extension))
						{
							auto& nunchuck = std::get<NunchuckData>(new_state.m_extension);
							std::array<uint8, 14> zero{};
							if (memcmp(zero.data(), data, zero.size()) == 0)
							{
								cemuLog_logDebug(LogType::Force, "Extension calibration data is zero");
								break;
							}

							nunchuck.calibration.zero.x = (uint16)data[0] << 2;
							nunchuck.calibration.zero.y = (uint16)data[1] << 2;
							nunchuck.calibration.zero.z = (uint16)data[2] << 2;
							nunchuck.calibration.zero.x |= (data[3] >> 4) & 0x3;
							nunchuck.calibration.zero.y |= (data[3] >> 2) & 0x3;
							nunchuck.calibration.zero.z |= data[3] & 0x3;

							nunchuck.calibration.gravity.x = (uint16)data[4] << 2;
							nunchuck.calibration.gravity.y = (uint16)data[5] << 2;
							nunchuck.calibration.gravity.z = (uint16)data[6] << 2;
							nunchuck.calibration.gravity.x |= (data[7] >> 4) & 0x3;
							nunchuck.calibration.gravity.y |= (data[7] >> 2) & 0x3;
							nunchuck.calibration.gravity.z |= data[7] & 0x3;

							nunchuck.calibration.max.x = data[8];
							nunchuck.calibration.max.y = data[11];

							nunchuck.calibration.min.x = data[9];
							nunchuck.calibration.min.y = data[12];

							nunchuck.calibration.center.x = data[10];
							nunchuck.calibration.center.y = data[13];
						}
					}
					else
					{
                        cemuLog_logDebug(LogType::Force,"Unhandled read data received");
                        continue;
					}

					update_report = true;
				}
				break;
            case kAcknowledge:
                {
                    new_state.buttons = *(uint16*)data & (~0x60E0);
                    data += 2;
                    const auto report_id = *data++;
                    const auto error = *data++;
                    if (report_id==kWriteMemory && wiimote.memory_ack_pending) wiimote.memory_ack_pending=false;
                    else
                    {
                        std::scoped_lock control(wiimote.speaker_control_mutex);
                        wiimote.speaker_control.Acknowledge(report_id,error);
                        publish_speaker_control(index,wiimote);
                    }
                    if (error)
                        cemuLog_logDebug(LogType::Force, "Error {:#x} from output report {:#x}", error, report_id);
                    break;
                }
			case kDataCore:
				{
					// 30 BB BB
					new_state.buttons = *(uint16*)data & (~0x60E0);
					data += 2;
					break;
				}
			case kDataCoreAcc:
				{
					// 31 BB BB AA AA AA
					new_state.buttons = *(uint16*)data & (~0x60E0);
					parse_acceleration(new_state, data);
					break;
				}
			case kDataCoreExt8:
				{
					log_raw_report("core_ext8");
					// 32 BB BB EE EE EE EE EE EE EE EE
					new_state.buttons = *(uint16*)data & (~0x60E0);
					data += 2;
					parse_extension_data(new_state, data);
					break;
				}
			case kDataCoreAccIR:
				{
					// 33 BB BB AA AA AA II II II II II II II II II II II II 
					new_state.buttons = *(uint16*)data & (~0x60E0);
					parse_acceleration(new_state, data);
					data += parse_ir(new_state, data);
					break;
				}
			case kDataCoreExt19:
				{
					log_raw_report("core_ext19");
					// 34 BB BB EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE 
					new_state.buttons = *(uint16*)data & (~0x60E0);
					data += 2;
					parse_extension_data(new_state, data);
					break;
				}
			case kDataCoreAccExt:
				{
					log_raw_report("core_acc_ext");
					// 35 BB BB AA AA AA EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE
					new_state.buttons = *(uint16*)data & (~0x60E0);
					parse_acceleration(new_state, data);
					parse_extension_data(new_state, data);
					break;
				}
			case kDataCoreIRExt:
				{
					log_raw_report("core_ir_ext");
					// 36 BB BB II II II II II II II II II II EE EE EE EE EE EE EE EE EE
					new_state.buttons = *(uint16*)data & (~0x60E0);
					data += 2;
					data += parse_ir(new_state, data);
					parse_extension_data(new_state, data);
					break;
				}
			case kDataCoreAccIRExt:
				{
					log_raw_report("core_acc_ir_ext");
					// 37 BB BB AA AA AA II II II II II II II II II II EE EE EE EE EE EE
					new_state.buttons = *(uint16*)data & (~0x60E0);
					parse_acceleration(new_state, data);
					data += parse_ir(new_state, data); // 10
					parse_extension_data(new_state, data);

					break;
				}
			case kDataExt:
				{
					// 3d EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE EE
					break;
				}
			default:
                cemuLog_logDebug(LogType::Force,"unhandled input packet id {} for wiimote {}", id, index);
			}

			if (new_state.m_motion_plus_active && new_state.m_motion_plus)
			{
				auto& mpls = *new_state.m_motion_plus;
				if (!mpls.factory_calibration_loaded && mpls.factory_calibration_attempts < 3 &&
					now >= new_state.mpls_settle_until && now >= new_state.next_mpls_calibration_read)
				{
					mpls.factory_calibration = {};
					mpls.factory_calibration_pending = true;
					++mpls.factory_calibration_attempts;
					new_state.next_mpls_calibration_read = now + std::chrono::seconds(2);
					send_read_packet(index, kRegisterMemory, kRegisterMotionPlusCalibration, 32);
					cemuLog_log(LogType::InputAPI, "Wiimote[{}] reading MotionPlus factory calibration attempt={}",
						index, mpls.factory_calibration_attempts);
				}
				const bool attached = new_state.m_motion_plus->extension_connected;
				if (!attached && new_state.m_extension.index() != 0)
				{
					new_state.m_extension = {};
					cemuLog_log(LogType::InputAPI, "Wiimote[{}] passthrough extension removed", index);
				}
				else if (attached && new_state.m_extension.index() == 0 && now >= new_state.next_passthrough_probe)
				{
					send_read_packet(index, kRegisterMemory, kRegisterPassthroughType, 4);
					new_state.next_passthrough_probe = now + std::chrono::seconds(1);
				}
			}
			if (id >= kDataCore)
				update_motion_sample(new_state);
			if (update_report)
				update_report_type(index, new_state, force_report);
			if(traceLifecycle && (wiimote.state.report_mode!=new_state.report_mode ||
				wiimote.state.m_motion_plus.has_value()!=new_state.m_motion_plus.has_value() ||
				wiimote.state.m_motion_plus_active!=new_state.m_motion_plus_active ||
				wiimote.state.m_extension.index()!=new_state.m_extension.index()))
				cemuLog_log(LogType::Force,"Wiimote lifecycle[{}]: input_mode={:02x} MotionPlus={} active={} extension={} calibrated={}",index,new_state.report_mode,new_state.m_motion_plus.has_value(),new_state.m_motion_plus_active,new_state.m_extension.index(),new_state.m_calibrated);

			std::unique_lock data_lock(wiimote.mutex);
			wiimote.state = std::move(new_state);
			data_lock.unlock();
			m_state_cache.Publish(index,wiimote.state);
			if (traceNunchuk)
			{
				const auto& state = wiimote.state;
				const auto* stick = std::get_if<NunchuckData>(&state.m_extension);
				const auto axis = stick ? stick->axis : glm::vec2{};
				const bool held = glm::length(axis) > 0.05f;
				auto& trace = nunchukTrace[index];
				if (now >= trace.next || held != trace.held)
				{
					trace.next = now + std::chrono::milliseconds(200);
					trace.held = held;
					cemuLog_log(LogType::Force,"Nunchuk physical[{}]: report={:02x} extension={} MotionPlus={} active={} raw=({},{}) axis=({:.4f},{:.4f})",
						index,static_cast<uint32>(id),state.m_extension.index(),state.m_motion_plus.has_value(),state.m_motion_plus_active,
						stick ? uint32(stick->raw_axis.x) : 0,stick ? uint32(stick->raw_axis.y) : 0,axis.x,axis.y);
				}
			}

		}

		lock.unlock();
		if (!receivedAnyPacket)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}

void WiimoteControllerProvider::parse_acceleration(WiimoteState& wiimote_state, const uint8*& data)
{
	glm::vec<3, uint16> raw_acc;
	// acc encoded in BB BB
	raw_acc.x = (*data >> 5) & 3; // 6|5 -> 1|0
	++data;
	raw_acc.y = (*data >> 4) & 2; // 5 -> 1
	raw_acc.z = (*data >> 5) & 2; // 6 -> 1
	++data;

	raw_acc.x |= (uint16)*data << 2;
	++data;
	raw_acc.y |= (uint16)*data << 2;
	++data;
	raw_acc.z |= (uint16)*data << 2;
	++data;

	wiimote_state.m_prev_acceleration = wiimote_state.m_acceleration;
	glm::vec3 acceleration = raw_acc;

	const auto& calib = wiimote_state.m_calib_acceleration;
	wiimote_state.m_acceleration = acceleration - glm::vec3(calib.zero);

	glm::vec3 tmp = calib.gravity;
	tmp -= calib.zero;
	acceleration = (wiimote_state.m_acceleration / tmp);

	const float pi_2 = (float)std::numbers::pi / 2.0f;
	wiimote_state.m_roll = std::atan2(acceleration.z, acceleration.x) - pi_2;
}

void WiimoteControllerProvider::rotate_ir(WiimoteState& wiimote_state)
{
	const float rot = wiimote_state.m_roll;
	if (rot == 0.0f)
		return;

	const float sin = std::sin(rot);
	const float cos = std::cos(rot);
	int i = -1;
	for (auto& dot : wiimote_state.ir_camera.dots)
	{
		i++;
		if (!dot.visible)
			continue;
		// move to center, rotate and move back
		dot.pos -= 0.5f;
		const auto x = dot.pos.x;
		const auto y = dot.pos.y;
		dot.pos.x = (x * cos) + (y * (-sin));
		dot.pos.y = (x * sin) + (y * cos);
		dot.pos += 0.5f;
	}
}

void WiimoteControllerProvider::calculate_ir_position(WiimoteState& wiimote_state)
{
	auto& ir = wiimote_state.ir_camera;
	ir.m_prev_position = ir.position;

	std::pair indices = ir.indices;
	if (ir.middle.x != 0)
	{
		const float last_angle = std::atan2(ir.middle.y, ir.middle.x);
		float best_distance = std::numeric_limits<float>::max();
		for (size_t i = 0; i < ir.dots.size(); ++i)
		{
			if (!ir.dots[i].visible)
				continue;

			for (size_t j = i + 1; j < ir.dots.size(); ++j)
			{
				if (!ir.dots[j].visible)
					continue;

				const auto mid = (ir.dots[i].pos + ir.dots[j].pos) / 2.0f;
				// check if angle is close enough to the last known one
				float angle = std::atan2(mid.y, mid.x);
				if (std::abs(last_angle - angle) > DegToRad(10.0f))
					continue;

				// check if distance between points is similar to last known distance
				const float distance = std::abs(ir.distance - glm::length(ir.dots[i].pos - ir.dots[j].pos));
				if (distance > 0.1f && distance > best_distance)
					continue;

				// found a new pair
				best_distance = distance;
				indices = {(sint32)i, (sint32)j};
			}
		}
	}

	if (ir.dots[indices.first].visible && ir.dots[indices.second].visible)
	{
		ir.prev_dots[indices.first] = ir.dots[indices.first];
		ir.prev_dots[indices.second] = ir.dots[indices.second];
		ir.position = (ir.dots[indices.first].pos + ir.dots[indices.second].pos) / 2.0f;

		ir.middle = ir.position;
		ir.distance = glm::length(ir.dots[indices.first].pos - ir.dots[indices.second].pos);
		ir.indices = indices;
		ir.missing_frames = 0;
		ir.m_positionVisibility = PositionVisibility::FULL;
	}
	else if (ir.dots[indices.first].visible)
	{
		ir.position = ir.middle + (ir.dots[indices.first].pos - ir.prev_dots[indices.first].pos);
		ir.missing_frames = 0;
		ir.m_positionVisibility = PositionVisibility::PARTIAL;
	}
	else if (ir.dots[indices.second].visible)
	{
		ir.position = ir.middle + (ir.dots[indices.second].pos - ir.prev_dots[indices.second].pos);
		ir.missing_frames = 0;
		ir.m_positionVisibility = PositionVisibility::PARTIAL;
	}
	else
	{
		// Real pointing stays usable through brief IR dropouts. Keep the last valid
		// solution alive for a few frames instead of making the icon disappear instantly.
		constexpr uint32 kIrHoldFrames = 8;
		if (ir.distance > 0.0f && ir.missing_frames < kIrHoldFrames)
		{
			++ir.missing_frames;
			ir.position = ir.middle;
			ir.m_positionVisibility = PositionVisibility::PARTIAL;
		}
		else
		{
			ir.missing_frames = kIrHoldFrames;
			ir.m_positionVisibility = PositionVisibility::NONE;
		}
	}

	ir.position.x = std::clamp(ir.position.x, 0.0f, 1.0f);
	ir.position.y = std::clamp(ir.position.y, 0.0f, 1.0f);

	const auto now = std::chrono::steady_clock::now();
	if (ir.m_positionVisibility != PositionVisibility::NONE)
	{
		const float dt = std::chrono::duration<float>(now - ir.pointer_timestamp).count();
		const auto filtered = ir.pointer_filter.Update(ir.position.x, ir.position.y, dt);
		ir.position = {filtered[0], filtered[1]};
	}
	else
		ir.pointer_filter.Reset();
	ir.pointer_timestamp = now;

	static uint32 s_irLogCounter = 0;
	if ((s_irLogCounter++ % 30) == 0)
	{
		cemuLog_log(
			LogType::InputAPI,
			"Wiimote IR vis=({},{},{},{}) pair=({},{}) pos=({:.3f},{:.3f}) mid=({:.3f},{:.3f}) dist={:.3f} state={}",
			ir.dots[0].visible, ir.dots[1].visible, ir.dots[2].visible, ir.dots[3].visible,
			ir.indices.first, ir.indices.second,
			ir.position.x, ir.position.y,
			ir.middle.x, ir.middle.y,
			ir.distance,
			static_cast<int>(ir.m_positionVisibility));
	}
}


sint32 WiimoteControllerProvider::parse_ir(WiimoteState& wiimote_state, const uint8* data)
{
	switch (wiimote_state.ir_camera.mode)
	{
	case kIRDisabled:
		wiimote_state.ir_camera.dots = {};
		wiimote_state.ir_camera.pointer_filter.Reset();
		return 0;
	case kBasicIR:
		{
			const auto ir = (BasicIR*)data;
			for (int i = 0; i < 2; ++i)
			{
				auto& dot1 = wiimote_state.ir_camera.dots[i * 2];
				auto& dot2 = wiimote_state.ir_camera.dots[i * 2 + 1];

				dot1.raw.x = ir[i].x1 | (ir[i].bits.x1 << 8); // 9|8
				dot1.raw.y = ir[i].y1 | (ir[i].bits.y1 << 8);
				dot1.size = 0;

				dot2.raw.x = ir[i].x2 | (ir[i].bits.x2 << 8);
				dot2.raw.y = ir[i].y2 | (ir[i].bits.y2 << 8);
				dot2.size = 0;

				dot1.visible = dot1.raw != glm::vec<2, uint16>(0x3ff, 0x3ff);
				if (dot1.visible)
					dot1.pos = glm::vec2(1.0f - dot1.raw.x / 1023.0f, (float)dot1.raw.y / 768.0f);
				else
					dot1.pos = {};

				dot2.visible = dot2.raw != glm::vec<2, uint16>(0x3ff, 0x3ff);
				if (dot2.visible)
					dot2.pos = glm::vec2(1.0f - dot2.raw.x / 1023.0f, (float)dot2.raw.y / 768.0f);
				else
					dot2.pos = {};
			}

			rotate_ir(wiimote_state);
			calculate_ir_position(wiimote_state);
			return sizeof(BasicIR) * 2;
		}
	case kExtendedIR:
		{
			const auto ir = (ExtendedIR*)data;
			for (int i = 0; i < 4; ++i)
			{
				auto& dot = wiimote_state.ir_camera.dots[i];
				dot.raw.x = ir[i].x;
				dot.raw.y = ir[i].y;

				dot.raw.x |= (uint16)ir[i].bits.x << 8; // 9|8
				dot.raw.y |= (uint16)ir[i].bits.y << 8; // 9|8

				dot.size = ir[i].bits.size;

				dot.visible = dot.raw != glm::vec<2, uint16>(0x3ff, 0x3ff);
				if (dot.visible)
					dot.pos = glm::vec2(1.0f - dot.raw.x / 1023.0f, (float)dot.raw.y / 768.0f);
				else
					dot.pos = {};
			}

			rotate_ir(wiimote_state);
			calculate_ir_position(wiimote_state);
			return sizeof(ExtendedIR) * 4;
		}
	default:
		cemu_assert(false);
		break;
	}
	return 0;
}

bool WiimoteControllerProvider::has_extension_data(const WiimoteState& state) const
{
	return state.m_motion_plus.has_value() || state.m_extension.index() != 0;
}

void WiimoteControllerProvider::parse_motion_plus_data(MotionPlusData& motion_plus, const uint8* data)
{
	const auto decode_axis = [](uint8 low, uint8 highWithFlags) -> uint16
	{
		// MotionPlus stores a 14-bit value with the upper 6 bits in bits 7..2 of the
		// trailing byte. Bits 1..0 are status flags and must not be folded into the reading.
		return static_cast<uint16>(low | ((static_cast<uint16>(highWithFlags & 0xFC)) << 6));
	};

	const uint16 yaw_raw = decode_axis(data[0], data[3]);
	const uint16 roll_raw = decode_axis(data[1], data[4]);
	const uint16 pitch_raw = decode_axis(data[2], data[5]);
	const glm::vec<3, uint16> raw{roll_raw, pitch_raw, yaw_raw};

	motion_plus.raw = raw;
	// Raw WM+ axis order is roll, pitch, yaw.
	motion_plus.slow_yaw = (data[3] & 0x02) != 0;
	motion_plus.slow_pitch = (data[3] & 0x01) != 0;
	motion_plus.slow_roll = (data[4] & 0x02) != 0;

	// Some report modes/remote combinations can feed 0xFF-filled extension payloads.
	// Don't treat that filler as real MotionPlus rotation data.
	if (raw == glm::vec<3, uint16>(0x3FFF, 0x3FFF, 0x3FFF))
	{
		motion_plus.zero_calibration.ResetPendingSamples();
		if (++motion_plus.filler_reports > 8)
		{
			motion_plus.orientation = {};
			motion_plus.angular_velocity = {};
		}
		return;
	}
	motion_plus.filler_reports = 0;

	// Bit 0 is reserved zero, including when no passthrough extension is attached.
	if ((data[5] & 0x03) != 0x02)
	{
		motion_plus.zero_calibration.ResetPendingSamples();
		return;
	}
	motion_plus.extension_connected = (data[4] & 0x01) != 0;
	const auto calibrated = motion_plus.zero_calibration.Update(
		{static_cast<float>(raw.x), static_cast<float>(raw.y), static_cast<float>(raw.z)},
		motion_plus.slow_roll && motion_plus.slow_pitch && motion_plus.slow_yaw);
	const glm::vec3 delta{calibrated[0], calibrated[1], calibrated[2]};
	motion_plus.orientation = delta;

	auto raw_to_rad = [](float value, bool slow_mode)
	{
		float degrees_per_second = value / 13.768f;
		if (!slow_mode)
			degrees_per_second *= (2000.0f / 440.0f);
		return glm::radians(degrees_per_second);
	};

	// Match Dolphin's dedicated MotionPlus handling sign convention and axis order:
	// X = pitch (-), Y = roll (+), Z = yaw (-).
	const float roll = raw_to_rad(delta.x, motion_plus.slow_roll);
	const float pitch = -raw_to_rad(delta.y, motion_plus.slow_pitch);
	const float yaw = -raw_to_rad(delta.z, motion_plus.slow_yaw);
	motion_plus.angular_velocity = {pitch, roll, yaw};

	static uint32 s_motionPlusLogCounter = 0;
	const float motion_plus_mag = glm::length(motion_plus.angular_velocity);
	if (motion_plus_mag > 0.02f || (s_motionPlusLogCounter++ % 40) == 0)
	{
		cemuLog_log(
			LogType::InputAPI,
			"MotionPlus native raw=({},{},{}) bias=({},{},{}) delta=({:.2f},{:.2f},{:.2f}) rad=({:.5f},{:.5f},{:.5f}) slow=({},{},{}) ext={}",
			raw.x, raw.y, raw.z,
			motion_plus.zero_calibration.bias[0], motion_plus.zero_calibration.bias[1], motion_plus.zero_calibration.bias[2],
			delta.x, delta.y, delta.z,
			pitch, roll, yaw,
			motion_plus.slow_roll, motion_plus.slow_pitch, motion_plus.slow_yaw,
			motion_plus.extension_connected);
	}
}

void WiimoteControllerProvider::parse_extension_data(WiimoteState& wiimote_state, const uint8* data)
{
	if (wiimote_state.m_motion_plus)
	{
		// The packet marker, not discovery of the attachment, selects gyro data.
		const bool motion_plus_report = (data[5] & 0x02) != 0;
		if (motion_plus_report)
		{
			parse_motion_plus_data(*wiimote_state.m_motion_plus, data);
			if ((data[5] & 3) == 2 && wiimote_state.m_motion_plus->filler_reports == 0)
				wiimote_state.m_motion_plus_active = true;
			return;
		}
	}

	if (std::holds_alternative<NunchuckData>(wiimote_state.m_extension) && wiimote_state.m_motion_plus)
	{
		auto& nunchuck = std::get<NunchuckData>(wiimote_state.m_extension);
		nunchuck.raw_axis.x = data[0];
		nunchuck.raw_axis.y = data[1];

		glm::vec<3, uint16> raw_acc{};
		raw_acc.x = static_cast<uint16>(data[2]) << 2;
		raw_acc.y = static_cast<uint16>(data[3]) << 2;
		raw_acc.z = static_cast<uint16>(data[4] & 0xFE) << 2;
		raw_acc.x |= (data[5] >> 4) & 0x2;
		raw_acc.y |= (data[5] >> 3) & 0x2;
		raw_acc.z |= (data[5] >> 6) & 0x6;

		nunchuck.c = (data[5] & 0x08) == 0;
		nunchuck.z = (data[5] & 0x04) == 0;

		auto& calib = nunchuck.calibration;
		if (nunchuck.raw_axis.x < nunchuck.calibration.center.x)
			nunchuck.axis.x = ((float)nunchuck.raw_axis.x - calib.min.x) /
				((float)nunchuck.calibration.center.x - calib.min.x + 0.012f) - 1.0f;
		else
			nunchuck.axis.x = (float)(nunchuck.raw_axis.x - nunchuck.calibration.center.x) /
				(nunchuck.calibration.max.x - nunchuck.calibration.center.x + 0.012f);

		if (nunchuck.raw_axis.y <= nunchuck.calibration.center.y)
			nunchuck.axis.y = ((float)nunchuck.raw_axis.y - calib.min.y) /
				((float)nunchuck.calibration.center.y - calib.min.y + 0.012f) - 1.0f;
		else
			nunchuck.axis.y = (float)(nunchuck.raw_axis.y - nunchuck.calibration.center.y) /
				(nunchuck.calibration.max.y - nunchuck.calibration.center.y);

		glm::vec3 acceleration = raw_acc;
		nunchuck.prev_acceleration = nunchuck.acceleration;
		nunchuck.acceleration = acceleration - glm::vec3(calib.zero);

		float acc[3]{-nunchuck.acceleration.x, -nunchuck.acceleration.z, nunchuck.acceleration.y};
		const auto grav = nunchuck.calibration.gravity - nunchuck.calibration.zero;
		auto tacc = nunchuck.acceleration;
		auto pacc = nunchuck.prev_acceleration;
		if (grav != glm::vec<3, uint16>{})
		{
			acc[0] /= (float)grav.x;
			acc[1] /= (float)grav.y;
			acc[2] /= (float)grav.z;
			tacc.x /= (float)grav.x;
			pacc.x /= (float)grav.x;
			tacc.y /= (float)grav.y;
			pacc.y /= (float)grav.y;
			tacc.z /= (float)grav.z;
			pacc.z /= (float)grav.z;
		}
		float zero3[3]{};
		float zero4[4]{};
		nunchuck.motion_sample =
			MotionSample(acc, glm::length(tacc - pacc), zero3, zero3, zero4);
		return;
	}

	if (std::holds_alternative<ClassicData>(wiimote_state.m_extension) && wiimote_state.m_motion_plus)
	{
		auto& classic = std::get<ClassicData>(wiimote_state.m_extension);
		classic.left_raw_axis.x = ((data[0] >> 1) & 0x1F) << 1;
		classic.left_raw_axis.y = ((data[1] >> 1) & 0x1F) << 1;
		classic.right_raw_axis.x =
			((data[0] >> 6) & 0x03) << 3 | ((data[1] >> 6) & 0x03) << 1 | ((data[2] >> 7) & 0x01);
		classic.right_raw_axis.y = data[2] & 0x1F;
		classic.raw_trigger.x = ((data[2] >> 5) & 0x03) << 3 | ((data[3] >> 5) & 0x07);
		classic.raw_trigger.y = data[3] & 0x1F;

		classic.buttons = 0;
		if ((data[0] & 0x01) == 0)
			classic.buttons |= (1 << kClassicButton_Up);
		if ((data[1] & 0x01) == 0)
			classic.buttons |= (1 << kClassicButton_Left);
		if ((data[4] & 0x80) == 0)
			classic.buttons |= (1 << kClassicButton_Right);
		if ((data[4] & 0x40) == 0)
			classic.buttons |= (1 << kClassicButton_Down);
		if ((data[4] & 0x20) == 0)
			classic.buttons |= (1 << kClassicButton_L);
		if ((data[4] & 0x10) == 0)
			classic.buttons |= (1 << kClassicButton_Minus);
		if ((data[4] & 0x08) == 0)
			classic.buttons |= (1 << kClassicButton_Home);
		if ((data[4] & 0x04) == 0)
			classic.buttons |= (1 << kClassicButton_Plus);
		if ((data[4] & 0x02) == 0)
			classic.buttons |= (1 << kClassicButton_R);
		if ((data[5] & 0x80) == 0)
			classic.buttons |= (1 << kClassicButton_ZL);
		if ((data[5] & 0x40) == 0)
			classic.buttons |= (1 << kClassicButton_B);
		if ((data[5] & 0x20) == 0)
			classic.buttons |= (1 << kClassicButton_Y);
		if ((data[5] & 0x10) == 0)
			classic.buttons |= (1 << kClassicButton_A);
		if ((data[5] & 0x08) == 0)
			classic.buttons |= (1 << kClassicButton_X);
		if ((data[5] & 0x04) == 0)
			classic.buttons |= (1 << kClassicButton_ZR);

		classic.left_axis = classic.left_raw_axis;
		classic.left_axis /= 63.0f;
		classic.left_axis = classic.left_axis * 2.0f - 1.0f;

		classic.right_axis = classic.right_raw_axis;
		classic.right_axis /= 31.0f;
		classic.right_axis = classic.right_axis * 2.0f - 1.0f;

		classic.trigger = classic.raw_trigger;
		classic.trigger /= 31.0f;
		return;
	}

	std::visit(
		overloaded
		{
			[](auto)
			{
			},
			[data](NunchuckData& nunchuck) mutable
			{
				nunchuck.raw_axis.x = *data;
				++data;
				nunchuck.raw_axis.y = *data;
				++data;

				glm::vec<3, uint16> raw_acc;
				raw_acc.x = static_cast<uint16>(*data) << 2;
				++data;
				raw_acc.y = static_cast<uint16>(*data) << 2;
				++data;
				raw_acc.z = static_cast<uint16>(*data) << 2;
				++data;
				nunchuck.z = (*data & 1) == 0;
				nunchuck.c = (*data & 2) == 0;

				raw_acc.x |= (*data >> 2) & 0x3;
				raw_acc.y |= (*data >> 4) & 0x3;
				raw_acc.z |= (*data >> 6) & 0x3;

				auto& calib = nunchuck.calibration;

				if (nunchuck.raw_axis.x < nunchuck.calibration.center.x)
					nunchuck.axis.x = ((float)nunchuck.raw_axis.x - calib.min.x) /
						((float)nunchuck.calibration.center.x - calib.min.x + 0.012f) - 1.0f;
				else
					nunchuck.axis.x = (float)(nunchuck.raw_axis.x - nunchuck.calibration.center.x) /
						(nunchuck.calibration.max.x - nunchuck.calibration.center.x + 0.012f);

				if (nunchuck.raw_axis.y <= nunchuck.calibration.center.y)
					nunchuck.axis.y = ((float)nunchuck.raw_axis.y - calib.min.y) /
						((float)nunchuck.calibration.center.y - calib.min.y + 0.012f) - 1.0f;
				else
					nunchuck.axis.y = (float)(nunchuck.raw_axis.y - nunchuck.calibration.center.y) /
						(nunchuck.calibration.max.y - nunchuck.calibration.center.y);

				glm::vec3 acceleration = raw_acc;
				nunchuck.prev_acceleration = nunchuck.acceleration;
				nunchuck.acceleration = acceleration - glm::vec3(calib.zero);

				float acc[3]{-nunchuck.acceleration.x, -nunchuck.acceleration.z, nunchuck.acceleration.y};
				const auto grav = nunchuck.calibration.gravity - nunchuck.calibration.zero;

				auto tacc = nunchuck.acceleration;
				auto pacc = nunchuck.prev_acceleration;
				if (grav != glm::vec<3, uint16>{})
				{
					acc[0] /= (float)grav.x;
					acc[1] /= (float)grav.y;
					acc[2] /= (float)grav.z;

					tacc.x /= (float)grav.x;
					pacc.x /= (float)grav.x;

					tacc.y /= (float)grav.y;
					pacc.y /= (float)grav.y;

					tacc.z /= (float)grav.z;
					pacc.z /= (float)grav.z;
				}
				float zero3[3]{};
				float zero4[4]{};

				nunchuck.motion_sample =
					MotionSample(acc, glm::length(tacc - pacc), zero3, zero3, zero4);
			},
			[data](ClassicData& classic) mutable
			{
				classic.left_raw_axis.x = *data & 0x3F;
				classic.right_raw_axis.x = (*data & 0xC0) >> 3;
				++data;

				classic.left_raw_axis.y = *data & 0x3F;
				classic.right_raw_axis.x |= (*data & 0xC0) >> 5;
				++data;

				classic.right_raw_axis.y = *data & 0x1F;
				classic.raw_trigger.x = (*data & 0x60) >> 2;
				classic.right_raw_axis.x |= (*data & 0x80) >> 7;
				++data;

				classic.raw_trigger.x |= (*data & 0xE0) >> 5;
				classic.raw_trigger.y = (*data & 0x1F);
				++data;

				classic.buttons = ~(*(uint16*)data);
				data += 2;

				classic.left_axis = classic.left_raw_axis;
				classic.left_axis /= 63.0f;
				classic.left_axis = classic.left_axis * 2.0f - 1.0f;

				classic.right_axis = classic.right_raw_axis;
				classic.right_axis /= 31.0f;
				classic.right_axis = classic.right_axis * 2.0f - 1.0f;

				classic.trigger = classic.raw_trigger;
				classic.trigger /= 31.0f;
			}
		},
		wiimote_state.m_extension);
}

void WiimoteControllerProvider::update_motion_sample(WiimoteState& wiimote_state)
{
	const auto now = std::chrono::high_resolution_clock::now();
	float delta_time = 0.005f;
	if (wiimote_state.m_last_motion_timestamp.time_since_epoch().count() != 0)
	{
		delta_time = std::chrono::duration_cast<std::chrono::duration<float>>(now - wiimote_state.m_last_motion_timestamp)
		                 .count();
		delta_time = std::max(delta_time, 0.001f);
	}
	wiimote_state.m_last_motion_timestamp = now;

	float acc[3]{-wiimote_state.m_acceleration.x, -wiimote_state.m_acceleration.z, wiimote_state.m_acceleration.y};
	const auto grav = wiimote_state.m_calib_acceleration.gravity - wiimote_state.m_calib_acceleration.zero;

	auto tacc = wiimote_state.m_acceleration;
	auto pacc = wiimote_state.m_prev_acceleration;
	if (grav != glm::vec<3, uint16>{})
	{
		acc[0] /= (float)grav.x;
		acc[1] /= (float)grav.y;
		acc[2] /= (float)grav.z;

		tacc.x /= (float)grav.x;
		pacc.x /= (float)grav.x;

		tacc.y /= (float)grav.y;
		pacc.y /= (float)grav.y;

		tacc.z /= (float)grav.z;
		pacc.z /= (float)grav.z;
	}

	if (wiimote_state.m_motion_plus)
	{
		const auto& motion_plus = *wiimote_state.m_motion_plus;
		if (grav.x && grav.y && grav.z)
		{
			// Normalize each sensor axis before reordering into KPAD coordinates.
			const auto& raw = wiimote_state.m_acceleration;
			wiimote_state.kpad_motion.Update(delta_time,
				{motion_plus.angular_velocity.x, motion_plus.angular_velocity.y, motion_plus.angular_velocity.z},
				{-raw.x / float(grav.x), -raw.z / float(grav.z), raw.y / float(grav.y)},
				motion_plus.zero_calibration.ready);
		}
		static uint32 s_updateMotionPlusLogCounter = 0;
		const float native_mag = glm::length(motion_plus.angular_velocity);
		if (native_mag > 0.02f || (s_updateMotionPlusLogCounter++ % 40) == 0)
		{
			cemuLog_log(
				LogType::InputAPI,
				"update_motion_sample mpls=true native=({:.5f},{:.5f},{:.5f}) dt={:.5f}",
				motion_plus.angular_velocity.x, motion_plus.angular_velocity.y, motion_plus.angular_velocity.z,
				delta_time);
		}
		wiimote_state.motion_handler.processMotionSample(
			delta_time, motion_plus.angular_velocity.x, motion_plus.angular_velocity.y,
			motion_plus.angular_velocity.z, acc[0], acc[1], acc[2]);
		// Native MotionPlus samples already go through zero-point correction in the
		// Wiimote parser. Reapplying the motion handler's gyro bias here can flatten
		// real twist before KPAD ever sees it.
		wiimote_state.motion_sample = wiimote_state.motion_handler.getMotionSample(false);
		float motion_sample_gyro[3]{};
		wiimote_state.motion_sample.getGyrometer(motion_sample_gyro);
		static uint32 s_motionSampleGyroLogCounter = 0;
		const float sample_mag = std::sqrt(
			motion_sample_gyro[0] * motion_sample_gyro[0] +
			motion_sample_gyro[1] * motion_sample_gyro[1] +
			motion_sample_gyro[2] * motion_sample_gyro[2]);
		if (sample_mag > 0.02f || (s_motionSampleGyroLogCounter++ % 40) == 0)
		{
			cemuLog_log(
				LogType::InputAPI,
				"MotionSample gyro=({:.5f},{:.5f},{:.5f})",
				motion_sample_gyro[0], motion_sample_gyro[1], motion_sample_gyro[2]);
		}
		return;
	}

	float zero3[3]{};
	float zero4[4]{};
	static uint32 s_updateMotionBaseLogCounter = 0;
	if ((s_updateMotionBaseLogCounter++ % 40) == 0)
	{
		cemuLog_log(
			LogType::InputAPI,
			"update_motion_sample mpls=false acc=({:.5f},{:.5f},{:.5f})",
			acc[0], acc[1], acc[2]);
	}
	wiimote_state.motion_sample = MotionSample(
		acc, glm::length(tacc - pacc), zero3, zero3, zero4);
}

void WiimoteControllerProvider::request_extension(size_t index)
{
	cemuLog_log(LogType::InputAPI, "Wiimote[{}] requesting extension type", index);
	// send_write_packet(index, kRegisterMemory, kRegisterExtensionEncrypted, { 0x00 });
	send_write_packet(index, kRegisterMemory, kRegisterExtension1, {0x55});
	send_write_packet(index, kRegisterMemory, kRegisterExtension2, {0x00});
	send_read_packet(index, kRegisterMemory, kRegisterExtensionType, 6);
}

void WiimoteControllerProvider::detect_motion_plus(size_t index)
{
	cemuLog_log(LogType::InputAPI, "Wiimote[{}] probing MotionPlus", index);
	send_read_packet(index, kRegisterMemory, kRegisterMotionPlusDetect, 6);
}

void WiimoteControllerProvider::set_motion_plus(size_t index, bool state)
{
	std::shared_lock lock(m_wiimotes[index].mutex);
	set_motion_plus(index, state, m_wiimotes[index].state);
}

void WiimoteControllerProvider::set_motion_plus(size_t index, bool state, const WiimoteState& state_override)
{
	if (state) {
		send_write_packet(index, kRegisterMemory, kRegisterMotionPlusInit, { 0x55 });
		uint8 mode = 0x04;
		if (std::holds_alternative<NunchuckData>(state_override.m_extension))
			mode = 0x05;
		else if (std::holds_alternative<ClassicData>(state_override.m_extension))
			mode = 0x07;
		cemuLog_log(LogType::InputAPI, "Wiimote[{}] enabling MotionPlus passthrough mode {:#x}", index, mode);
		send_write_packet(index, kRegisterMemory, kRegisterMotionPlusEnable, { mode });
	}
	else
	{
		cemuLog_log(LogType::InputAPI, "Wiimote[{}] disabling MotionPlus", index);
		send_write_packet(index, kRegisterMemory, kRegisterExtension1, { 0x55 });
	}
}


void WiimoteControllerProvider::writer_thread()
{
	WiimoteOutputWait outputWait;
	const bool traceSpeaker = std::getenv("CEMU_WIIMOTE_SPEAKER_TRACE") != nullptr;
	const bool traceControl = std::getenv("CEMU_WIIMOTE_LIFECYCLE_TRACE") != nullptr;
	struct Trace { std::chrono::steady_clock::time_point last{}, window{}; uint32 packets{}, bytes{}; double maxGapMs{}; };
	std::unordered_map<size_t,Trace> speakerTrace;
	std::chrono::steady_clock::duration previousWait{};
	SetThreadName("Wiimote-writer");
	while (m_threadsRunning.load(std::memory_order_relaxed))
	{
		std::unique_lock writer_lock(m_writer_mutex);
		while (m_write_queue.empty())
		{
			if (m_writer_cond.wait_for(writer_lock, std::chrono::milliseconds(250)) == std::cv_status::timeout)
			{
				if (!m_threadsRunning.load(std::memory_order_relaxed))
					return;
			}
		}
		// Reader/discovery acquire device before queue. Never wait for the device
		// while holding the queue lock, or hotplug can deadlock both threads.
		const bool streaming = std::any_of(m_write_queue.begin(), m_write_queue.end(), [](const auto& packet) {
			return !packet.second.empty() && WiimoteOutputSchedule::IsAudio(packet.second[0]);
		});
		writer_lock.unlock();
		const auto lockStart = std::chrono::steady_clock::now();
		auto device_lock = m_outputGate.LockOutput(m_device_mutex, streaming);
		writer_lock.lock();
		const auto selectStart = std::chrono::steady_clock::now();

		auto index = (size_t)-1;
		std::vector<uint8> data;
		uint32 controlGeneration=0;unsigned controlStep=0;
		for(auto it=m_write_queue.begin();it!=m_write_queue.end();)
		{
			if(it->controlGeneration && it->first<m_wiimotes.size())
			{
				auto& remote=m_wiimotes[it->first];std::scoped_lock control(remote.speaker_control_mutex);
				remote.speaker_control.Poll(std::chrono::steady_clock::now());publish_speaker_control(it->first,remote);
				if(!remote.speaker_control.Pending() || it->controlGeneration!=remote.speaker_control.generation){it=m_write_queue.erase(it);continue;}
			}
			++it;
		}

		// get first packet of device which is ready to be sent
		const auto now = std::chrono::high_resolution_clock::now();
		WiimoteOutputSchedule::Wake wake;
		const auto selected = WiimoteOutputSchedule::Select(m_write_queue, m_wiimotes.size(), [&](size_t device, uint8 report, const OutputPacket& packet) {
			auto& remote = m_wiimotes[device];
			std::scoped_lock control(remote.speaker_control_mutex);
			const auto time=std::chrono::steady_clock::now();
			if(remote.memory_ack_pending && time>=remote.memory_ack_deadline) remote.memory_ack_pending=false;
			if(packet.controlGeneration)
			{
				if(remote.memory_ack_pending || !remote.speaker_control.Ready(packet.controlGeneration,packet.controlStep,time)) { wake.PollControl(time); return false; }
			}
			else if(remote.speaker_control.Pending() || (report==kWriteMemory && remote.memory_ack_pending)) { wake.PollControl(time); return false; }
			if(packet.restoreReport && report==kType)
			{
				const bool controlReady = now >= remote.data_ts + std::chrono::milliseconds(m_state_cache.Read(device).packetDelay);
				if(WiimoteReportRecovery::Ready(report,true,remote.memory_ack_pending,controlReady))return true;
				wake.PollControl(time);return false;
			}
			if (WiimoteOutputSchedule::IsAudio(report)) return wake.AudioReady(remote.speaker_clock.Deadline(), time);
			const bool ready = now >= remote.data_ts + std::chrono::milliseconds(m_state_cache.Read(device).packetDelay);
			if (!ready) wake.PollControl(time);
			return ready;
		});
		if (selected != m_write_queue.end())
		{
			index = selected->first;
			controlGeneration=selected->controlGeneration;controlStep=selected->controlStep;
			data = std::move(selected->second);
			m_write_queue.erase(selected);
		}
		writer_lock.unlock();

		bool wrote = false;
		const bool attempted = index != (size_t)-1 && !data.empty();
		if (attempted)
		{
			auto& wiimote = m_wiimotes[index];
			if (!wiimote.device)
				continue;
			if (wiimote.rumble)
				data[1] |= 1;
			const auto writeStart=std::chrono::steady_clock::now();
			if (data[0] == kSpeakerData)
				WiimoteSpeakerTrace::Timing(index, previousWait, selectStart - lockStart, writeStart - selectStart);
			const bool written=data[0]==kSpeakerData?wiimote.device->write_audio_data(data,wiimote.speaker_compatibility):wiimote.device->write_data(data);
			if (traceControl && (data[0] == kType || data[0] == kReadMemory || data[0] == kWriteMemory))
			{
				const auto at = [&](size_t offset) -> uint32 { return offset < data.size() ? data[offset] : 0; };
				cemuLog_log(LogType::Force,"Wiimote control[{}]: report={:02x} written={} data={:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
					index,data[0],written,at(1),at(2),at(3),at(4),at(5),at(6));
			}
			WiimoteSpeakerTrace::Packet(WiimoteSpeakerTrace::Stage::Wire,index,written,data,
				static_cast<uint32>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-writeStart).count()));
			if (!written)
			{
				if(controlGeneration){std::scoped_lock control(wiimote.speaker_control_mutex);wiimote.speaker_control.Fail();publish_speaker_control(index,wiimote);}
				m_state_cache.Clear(index);
				wiimote.io_failed = true;
				wiimote.rumble = false;
			}
			else
			{
				wrote = true;
				const auto sent = std::chrono::steady_clock::now();
				wiimote.status_refresh.Sent(data[0]);
				if(controlGeneration)
				{
					std::scoped_lock control(wiimote.speaker_control_mutex);
					wiimote.speaker_control.Written(controlGeneration,controlStep,sent);
				}
				else if(data[0]==kWriteMemory){wiimote.memory_ack_pending=true;wiimote.memory_ack_deadline=sent+std::chrono::milliseconds(500);}
				// Audio intervals are between submissions; USB write latency is part of that interval.
				{
					std::scoped_lock control(wiimote.speaker_control_mutex);
					WiimoteOutputSchedule::Sent(data[0], data[1] >> 3, std::chrono::high_resolution_clock::now(), writeStart, wiimote.data_ts, wiimote.speaker_clock);
				}
				if (data[0] == kSpeakerData)
				{
					if (traceSpeaker)
					{
						auto& trace = speakerTrace[index];
						if (trace.last != std::chrono::steady_clock::time_point{} && sent - trace.last < std::chrono::milliseconds(100))
							trace.maxGapMs = std::max(trace.maxGapMs, std::chrono::duration<double,std::milli>(sent - trace.last).count());
						trace.last = sent; ++trace.packets; trace.bytes += data[1] >> 3;
						if (sent - trace.window >= std::chrono::seconds(1))
						{
							cemuLog_log(LogType::Force, "Wiimote speaker transport[{}]: packets={} bytes={} max_active_gap_ms={:.3f}", index, trace.packets, trace.bytes, trace.maxGapMs);
							trace.window = sent; trace.packets = trace.bytes = 0; trace.maxGapMs = 0;
						}
					}
				}
			}
		}
		device_lock.unlock();
		// Re-select immediately after I/O; sample deadlines already enforce packet spacing.
		if (wrote) { previousWait = {}; continue; }

		const auto waitStart = std::chrono::steady_clock::now();
		outputWait.WaitUntil(attempted ? waitStart + std::chrono::milliseconds(1) : wake.Deadline(waitStart), streaming);
		previousWait = std::chrono::steady_clock::now() - waitStart;
	}
}

void WiimoteControllerProvider::calibrate(size_t index)
{
	send_read_packet(index, kEEPROMMemory, kRegisterCalibration, 8);
}

void WiimoteControllerProvider::update_report_type(size_t index, WiimoteState& state, bool force)
{
	const auto previousIR=state.ir_camera.mode;
	set_ir_camera(index, state);

	const bool extension = has_extension_data(state);
	const bool motion_plus_only = state.m_motion_plus.has_value() && state.m_extension.index() == 0;
	const bool ir = state.ir_camera.mode != kIRDisabled;
	const bool motion = true; // UseMotion();

	InputReportId report_type;
	// Real Wii Remote Plus devices can return 0xFF-filled MotionPlus bytes in the mixed
	// Acc+Ext report mode. Prefer pure extension report modes when MotionPlus is the only
	// active attachment so we get real gyro data instead of filler.
	if (motion_plus_only && ir)
		report_type = kDataCoreAccIRExt;
	else if (motion_plus_only)
		report_type = kDataCoreExt19;
	else if (extension && ir && motion)
		report_type = kDataCoreAccIRExt;
	else if (extension && ir)
		report_type = kDataCoreIRExt;
	else if (extension && motion)
		report_type = kDataCoreAccExt;
	else if (ir && motion)
		report_type = kDataCoreAccIR;
	else if (extension)
		report_type = kDataCoreExt19;
	else if (ir)
		report_type = kDataCoreAccIR;
	else if (motion)
		report_type = kDataCoreAcc;
	else
		report_type = kDataCore;

	const bool restoreOnly=force && state.report_mode==report_type && previousIR==state.ir_camera.mode;
	if(!WiimoteSpeakerProtocol::RestoreMode(state.report_mode,report_type,force)) return;
	cemuLog_logDebug(LogType::Force,"Setting report type to {}", report_type);
	send_packet(index, {kType, 0x04, report_type},restoreOnly);

}

IRMode WiimoteControllerProvider::set_ir_camera(size_t index, WiimoteState& wiimote_state)
{
	const IRMode mode = has_extension_data(wiimote_state) ? kBasicIR : kExtendedIR;

	if (wiimote_state.ir_camera.mode == mode)
		return mode;

	wiimote_state.ir_camera.mode = mode;

	const uint8_t data = 0x04;
	send_packet(index, {kIR, data});
	send_packet(index, {kIR2, data});
	{
		send_write_packet(index, kRegisterMemory, kRegisterIR, {0x08});
		send_write_packet(index, kRegisterMemory, kRegisterIRSensitivity1,
		                  {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xaa, 0x00, 0x64});
		send_write_packet(index, kRegisterMemory, kRegisterIRSensitivity2, {0x63, 0x03});
		send_write_packet(index, kRegisterMemory, kRegisterIRMode, {(uint8)mode});
		send_write_packet(index, kRegisterMemory, kRegisterIR, {0x08});
	}

	return mode;
}

void WiimoteControllerProvider::send_packet(size_t index, std::vector<uint8> data,bool restoreReport)
{
	cemu_assert(data.size() > 1);

	std::unique_lock lock(m_writer_mutex);
	if(restoreReport)WiimoteReportRecovery::Insert(m_write_queue,index,std::move(data));
	else m_write_queue.emplace_back(index, std::move(data));
	m_writer_cond.notify_one();
}

void WiimoteControllerProvider::send_read_packet(size_t index, MemoryType type, RegisterAddress address, uint16 size)
{
	std::vector<uint8> data(7);
	data[0] = kReadMemory;
	data[1] = type;
	*(betype<uint32>*)(data.data() + 2) = (address & 0xFFFFFF) << 8; // only uint24
	*(betype<uint16>*)(data.data() + 2 + 3) = size;

	send_packet(index, std::move(data));
}

void WiimoteControllerProvider::send_write_packet(size_t index, MemoryType type, RegisterAddress address,
                                                  const std::vector<uint8>& data)
{
	cemu_assert(data.size() <= 16);
	std::vector<uint8> packet(6 + 16);
	packet[0] = kWriteMemory;
	packet[1] = type;
	*(betype<uint32>*)(packet.data() + 2) = (address & 0xFFFFFF) << 8; // only uint24
	*(packet.data() + 2 + 3) = (uint8)data.size();
	std::copy(data.begin(), data.end(), packet.data() + 2 + 3 + 1);
	send_packet(index, std::move(packet));
}
