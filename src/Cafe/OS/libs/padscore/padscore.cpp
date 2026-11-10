#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/padscore/padscore.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_SystemInfo.h"
#include "WindowSystem.h"
#include "Cafe/OS/libs/padscore/WpadLifecycle.h"
#include "Cafe/OS/libs/padscore/WpadSpeakerControl.h"
#include "Cafe/OS/libs/padscore/MplsProcessing.h"
#include "input/InputManager.h"
#include "input/ControllerFactory.h"
#include "input/emulated/WiimoteController.h"
#include "input/api/Wiimote/NativeWiimoteController.h"
#include "input/api/Wiimote/WiimoteBattery.h"

#include "YamahaAdpcm.h"
#include "input/api/Wiimote/WiimoteSpeakerTrace.h"
#include <bit>

constinit COSStaticData s_padscoreStaticData;

// KPAD

enum class KPAD_ERROR : sint32
{
	NONE = 0,
	NO_SAMPLE_DATA = -1,
	NO_CONTROLLER = -2,
	NOT_INITIALIZED = -5,
};

// WPAD (Wii Controller stuff)

enum WPADRumble
{
	kStopRumble = 0,
	kStartRumble = 1,
};

enum class WPADBatteryLevel : uint8
{
	FULL = 4,
};

enum class WPADLed : uint8
{
	CHAN0 = (1 << 0),
	CHAN1 = (1 << 1),
	CHAN2 = (1 << 2),
	CHAN3 = (1 << 3),
};

namespace padscore 
{
	static bool g_mpls_work_requested = false;
	static MEMPTR<void> g_mpls_workarea;

	static std::shared_ptr<WPADController> GetConnectedController(uint32 channel)
	{
		auto controller = InputManager::instance().get_wpad_controller(channel);
		return controller && controller->is_guest_connected() ? controller : nullptr;
	}
	static std::shared_ptr<NativeWiimoteController> NativeRemote(uint32 channel)
	{

		if (const auto pad = GetConnectedController(channel))
			for (const auto& source : pad->controllers_snapshot())
				if (auto native = std::dynamic_pointer_cast<NativeWiimoteController>(source); native && native->is_connected()) return native;
		return {};
	}
	static uint8 BatteryLevel(uint32 channel)
	{

		if (const auto native = NativeRemote(channel)) { const auto percent = native->battery_percent(); return percent ? WiimoteBattery::Bars(*percent) : 0; }
		return GetConnectedController(channel) ? 4 : 0;
	}
	static uint8 s_speakerVolume = 64;
	static bool s_motorsEnabled = true;
	static std::unordered_map<std::string,float> s_motorStrengths;
	uint8 WPADGetSpeakerVolume()
	{

		for (uint32 i=0;i<InputManager::kMaxWPADControllers;++i) if (const auto remote=NativeRemote(i)) return remote->get_speaker_volume();
		return s_speakerVolume;
	}
	void WPADSetSpeakerVolume(uint8 volume)
	{
		volume=std::min<uint8>(127,volume);

		s_speakerVolume=volume;
		for(uint32 i=0;i<InputManager::kMaxWPADControllers;++i) if(auto remote=NativeRemote(i))
		{
			remote->set_speaker_volume(volume);
			if(auto pad=GetConnectedController(i)) InputManager::instance().save(pad->player_index());
		}
	}
	bool WPADMotorsEnabled()
	{

		bool found = false;
		for (uint32 i = 0; i < InputManager::kMaxWPADControllers; ++i)
			if (const auto pad = GetConnectedController(i))
				for (const auto& device : pad->controllers_snapshot())
					if (device->has_rumble()) { found = true; if (device->get_settings().rumble > 0) return true; }
		return found ? false : s_motorsEnabled;
	}
	void WPADEnableMotor(bool enabled)
	{

		s_motorsEnabled=enabled;
		for(uint32 i=0;i<InputManager::kMaxWPADControllers;++i) if(auto pad=GetConnectedController(i))
		{
			for(const auto& device:pad->controllers_snapshot()) if(device->has_rumble())
			{
				auto& strength=s_motorStrengths[std::string(device->api_name())+":"+device->uuid()];
				if(device->get_settings().rumble>0) strength=device->get_settings().rumble;
				device->set_rumble(enabled ? (strength>0?strength:1.0f):0.0f);
				if(!enabled) device->stop_rumble();
			}
			InputManager::instance().save(pad->player_index());
		}
	}
	static MEMPTR<void> s_syncCallback;
	static bool s_syncActive{},s_syncStarted{},s_syncAcked{};
	static uint32 s_syncBeforeMask{};
	static std::chrono::steady_clock::time_point s_syncUntil;
	MEMPTR<void> WPADSetSyncDeviceCallback(MEMPTR<void> callback){auto old=s_syncCallback;s_syncCallback=callback;return old;}
	bool WPADStartSyncDevice()
	{
		if(s_syncActive) return false;
		if(!InputManager::instance().is_api_available(InputAPI::Wiimote)) return false;
		InputManager::instance().request_wiimote_pairing();
		s_syncBeforeMask=0;for(uint32 i=0;i<InputManager::kMaxWPADControllers;++i) if(GetConnectedController(i))s_syncBeforeMask|=1u<<i;
		s_syncActive=true;s_syncStarted=true;s_syncAcked=false;s_syncUntil=std::chrono::steady_clock::now()+std::chrono::seconds(30);return true;
	}
	bool WPADCancelSyncDevice(){s_syncActive=false;s_syncAcked=true;return true;}
	bool WPADIsBusyForSync(){return s_syncActive;}
	static void PollSync()
	{
		if(s_syncStarted){s_syncStarted=false;if(s_syncCallback)PPCCoreCallback(s_syncCallback,0,0);}
		if(!s_syncActive && !s_syncAcked) return;
		uint32 mask=0;for(uint32 i=0;i<InputManager::kMaxWPADControllers;++i)if(GetConnectedController(i))mask|=1u<<i;
		const uint32 count=std::popcount(mask & ~s_syncBeforeMask);
		const bool busy=count==0 && std::chrono::steady_clock::now()<s_syncUntil;
		if(!busy || s_syncAcked){s_syncActive=s_syncAcked=false;if(s_syncCallback)PPCCoreCallback(s_syncCallback,1,count);}
	}
	uint32 WPADSaveConfig(){InputManager::instance().save();return 1;}
	void WPADDeleteControllerOrder() {} // Host assignments stay stable while native discovery runs.

	enum WPADState_t
	{
		kWPADStateMaster = 0,
		kWPADStateShutdown,
		kWPADStateInitializing,
		kWPADStateAcquired,
		kWPADStateReleased,
	};
	KPADUnifiedWpadStatus_t* g_kpad_ringbuffer = nullptr;
	uint32 g_kpad_ringbuffer_length = 0;
	bool g_wpad_callback_by_kpad = false;

	WPADState_t g_wpad_state = kWPADStateMaster;

	struct {
		CafeStaticVar<coreinit::OSAlarm_t> alarm{s_padscoreStaticData};
		bool kpad_initialized = false;

		struct WPADData
		{
			MEMPTR<void> extension_callback;
			MEMPTR<void> connectCallback;
			MEMPTR<void> sampling_callback;
			MEMPTR<void> dpd_callback;
			bool dpd_enabled = true;
			bool mpls_requested = false;
			WpadSpeakerControl speaker;
			MEMPTR<void> speaker_callback;
			bool speaker_submitted=false;


			WpadLifecycle::Connection connection;
			std::optional<uint8> mpls_mode;
			MEMPTR<void> mpls_callback;
			bool mpls_config_pending = false;
			bool mpls_config_inflight = false;
			uint8 mpls_last_status = 0;
			MplsProcessing mpls_processing;
			MEMPTR<uint32be> pending_attached;
			MEMPTR<void> pending_attached_callback;
			bool attached_query_pending = false;

			BtnRepeat btn_repeat{};
		} controller_data[InputManager::kMaxWPADControllers] = {};

		int max_controllers = kWPADMaxControllers; // max bt controllers?
	} g_padscore;

	static WPADDeviceType GetReportedDeviceType(uint32 channel, const std::shared_ptr<WPADController>& controller)
	{
		const auto device_type = controller->get_device_type();
		if (!controller->is_mpls_attached())
			return device_type;

		if (channel < InputManager::kMaxWPADControllers &&
			g_padscore.controller_data[channel].mpls_mode.has_value())
		{
			const uint8 mode = *g_padscore.controller_data[channel].mpls_mode;
			if (mode != 0)
			{
				const uint8 status = WpadLifecycle::MotionPlusStatus(mode, controller->is_mpls_ready(), device_type);
				if (status == 5) return kWAPDevMPLSFreeStyle;
				if (status == 7) return kWAPDevMPLSClassic;
				if (status == 4) return kWAPDevMPLS;
			}
			if (device_type == kWAPDevMPLSFreeStyle) return kWAPDevFreestyle;
			if (device_type == kWAPDevMPLSClassic) return kWAPDevClassic;
			return kWAPDevCore;
		}
		// Work-area and attachment queries do not enable MotionPlus for a game.
		const bool report_mpls = channel < InputManager::kMaxWPADControllers &&
			g_padscore.controller_data[channel].mpls_requested;
		if (report_mpls)
			return device_type;

		switch (device_type)
		{
		case kWAPDevMPLS:
			return kWAPDevCore;
		case kWAPDevMPLSFreeStyle:
			return kWAPDevFreestyle;
		case kWAPDevMPLSClassic:
			return kWAPDevClassic;
		default:
			return device_type;
		}
	}

	static bool HasSpeaker(uint32 channel)
	{
		const auto controller = GetConnectedController(channel);
		return controller && controller->type() == EmulatedController::Wiimote;
	}

	sint32 WPADControlSpeaker(uint32 channel, uint32 command, MEMPTR<void> callback)
	{
		sint32 result = WPAD_ERR_NO_CONTROLLER;
		if (channel < InputManager::kMaxWPADControllers)
		{
			auto& data = g_padscore.controller_data[channel];
			result = data.speaker.Queue(HasSpeaker(channel), command);
			if (result == WPAD_ERR_NONE) data.speaker_callback = callback;
		}
		if (result != WPAD_ERR_NONE && callback)
			coreinitAsyncCallback_add(callback.GetMPTR(), 2, channel, static_cast<uint32>(result));
		return result;
	}

	bool WPADIsSpeakerEnabled(uint32 channel)
	{
		return channel < InputManager::kMaxWPADControllers && HasSpeaker(channel) &&
			g_padscore.controller_data[channel].speaker.enabled;
	}

	bool WPADCanSendStreamData(uint32 channel)
	{

		const auto native=NativeRemote(channel);return native && native->can_send_speaker();
	}

	sint32 WPADSendStreamData(uint32 channel, MEMPTR<const void> data, uint32 length)
	{
		if (!HasSpeaker(channel) || !data) return WPAD_ERR_NO_CONTROLLER;
		if(!length || length>20) return -4;

		const auto native=NativeRemote(channel);return native && native->send_speaker({static_cast<const uint8*>(data.GetPtr()),length}) ? 0 : -2;
	}

	uint32 WENCGetEncodeData(MEMPTR<sint32be> params,uint32 mode,MEMPTR<const sint16be> samples,uint32 count,MEMPTR<uint8> output)
	{
		if(!params || !samples || !output || count>65536 || mode>1) return 0;
		YamahaAdpcm encoder;if(mode){encoder.predictor=params[0];encoder.step=params[1];}
		const auto before=encoder;
		for(uint32 i=0;i<count;++i){const uint8 nibble=encoder.Encode(samples[i]);if(!(i&1))output[i/2]=nibble<<4;else output[i/2]|=nibble;}
		WiimoteSpeakerTrace::Source(params.GetMPTR(),mode,before.predictor,before.step,encoder.predictor,encoder.step,
			{reinterpret_cast<const uint8*>(samples.GetPtr()),count*2},{output.GetPtr(),(count+1)/2});
		params[0]=encoder.predictor;params[1]=encoder.step;return count;
	}
}


#pragma region WPAD

void padscoreExport_WPADGetStatus(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::InputAPI, "WPADGetStatus()");
	uint32 status = 1;
	osLib_returnFromFunction(hCPU, status);
}

void padscoreExport_WPADGetBatteryLevel(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel,0);
	cemuLog_log(LogType::InputAPI, "WPADGetBatteryLevel()");
	osLib_returnFromFunction(hCPU, padscore::BatteryLevel(channel));
}

void padscoreExport_WPADProbe(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(type, uint32be, 1);

	if(const auto controller = padscore::GetConnectedController(channel))
	{
		const auto device_type = padscore::GetReportedDeviceType(channel, controller);
		if(type)
			*type = device_type;

		cemuLog_log(LogType::InputAPI, "WPADProbe({}) -> type={} rawType={} mpls={}", channel, (uint32)device_type, (uint32)controller->get_device_type(), controller->is_mpls_attached());

		osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
	}
	else
	{
		if(type)
			*type = 253;

		cemuLog_log(LogType::InputAPI, "WPADProbe({}) -> no controller", channel);

		osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
	}
}

typedef struct
{
	uint32be dpd;
	uint32be speaker;
	uint32be attach;
	uint32be lowBat;
	uint32be nearempty;
	betype<WPADBatteryLevel> batteryLevel;
	betype<WPADLed> led;
	uint8 protocol;
	uint8 firmware;
}WPADInfo_t;

static_assert(sizeof(WPADInfo_t) == 0x18); // unsure

void padscoreExport_WPADGetInfoAsync(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamStructPtr(wpadInfo, WPADInfo_t, 1);
	ppcDefineParamMPTR(callbackFunc, 2);
	cemuLog_log(LogType::InputAPI, "WPADGetInfoAsync({}, 0x{:08x}, 0x{:08x})", channel, memory_getVirtualOffsetFromPointer(wpadInfo), callbackFunc);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = padscore::GetConnectedController(channel))
		{
			wpadInfo->dpd = controller->has_position() ? TRUE : FALSE;
			wpadInfo->speaker = padscore::WPADIsSpeakerEnabled(channel);
			wpadInfo->attach = controller->get_device_type() == kWAPDevCore ? FALSE : TRUE;
			wpadInfo->lowBat = padscore::BatteryLevel(channel) <= 1 ? TRUE : FALSE;
			wpadInfo->nearempty = FALSE;
			wpadInfo->batteryLevel = static_cast<WPADBatteryLevel>(padscore::BatteryLevel(channel));
			wpadInfo->led = static_cast<WPADLed>(1 << (channel % 4));

			if(callbackFunc != MPTR_NULL)
				coreinitAsyncCallback_add(callbackFunc, 2, channel, (uint32)KPAD_ERROR::NONE);

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}
	else
	{
		debugBreakpoint();
	}

	if (callbackFunc != MPTR_NULL)
		coreinitAsyncCallback_add(callbackFunc, 2, channel, (uint32)KPAD_ERROR::NO_CONTROLLER);

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADRead(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(wpadStatus, WPADStatus_t, 1);
	cemuLog_log(LogType::InputAPI, "WPADRead({}, {:x})", channel, memory_getVirtualOffsetFromPointer(wpadStatus));

	if (channel < InputManager::kMaxWPADControllers)
	{
		if(const auto controller = padscore::GetConnectedController(channel) )
		{
			controller->WPADRead(wpadStatus);
			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}
	else
	{
		debugBreakpoint();
	}
	if (wpadStatus)
		wpadStatus->err = WPAD_ERR_NO_CONTROLLER;
	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADSetDataFormat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(fmt, 1);
	cemuLog_log(LogType::InputAPI, "WPADSetDataFormat({}, {})", channel, fmt);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = padscore::GetConnectedController(channel))
		{
			controller->set_data_format((WPADDataFormat)fmt);

			padscore::g_padscore.controller_data[channel].mpls_requested = (WPADDataFormat)fmt == kDataFormat_MPLS;
		}
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_WPADGetDataFormat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	cemuLog_log(LogType::InputAPI, "WPADGetDataFormat({})", channel);

	sint32 dataFormat = kDataFormat_CORE;
	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = padscore::GetConnectedController(channel))
			dataFormat = controller->get_data_format();
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, dataFormat);
}

void padscoreExport_WPADGetInfo(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamStructPtr(wpadInfo, WPADInfo_t, 1);
	cemuLog_log(LogType::InputAPI, "WPADGetInfo({}, 0x{:08x})", channel, memory_getVirtualOffsetFromPointer(wpadInfo));

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = padscore::GetConnectedController(channel))
		{
			wpadInfo->dpd = controller->has_position() ? TRUE : FALSE;
			wpadInfo->speaker = padscore::WPADIsSpeakerEnabled(channel);
			wpadInfo->attach = controller->get_device_type() == kWAPDevCore ? FALSE : TRUE;
			wpadInfo->lowBat = padscore::BatteryLevel(channel) <= 1 ? TRUE : FALSE;
			wpadInfo->nearempty = FALSE;
			wpadInfo->batteryLevel = static_cast<WPADBatteryLevel>(padscore::BatteryLevel(channel));
			wpadInfo->led = static_cast<WPADLed>(1 << (channel % 4));

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}


void padscoreExport_WPADIsMotorEnabled(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::InputAPI, "WPADIsMotorEnabled()");
	osLib_returnFromFunction(hCPU, padscore::WPADMotorsEnabled());
}

void padscoreExport_WPADControlMotor(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(command, 1);
	cemuLog_log(LogType::InputAPI, "WPADControlMotor({}, {})", channel, command);

	
	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = padscore::GetConnectedController(channel))
		{
			if (command == kStartRumble && padscore::WPADMotorsEnabled())
				controller->start_rumble();
			else
				controller->stop_rumble();
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, 0);
}
#pragma endregion



#pragma region KPAD
void padscoreExport_KPADGetUnifiedWpadStatus(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(status, KPADUnifiedWpadStatus_t, 1);
	ppcDefineParamU32(count, 2);

	cemuLog_log(LogType::InputAPI, "KPADGetUnifiedWpadStatus({}, 0x{:08x}, 0x{:x})", channel, MEMPTR<void>(status).GetMPTR(), count);

	if (channel < InputManager::kMaxWPADControllers)
	{
		memset(status, 0x00, sizeof(KPADUnifiedWpadStatus_t) * count);

		if (const auto controller = padscore::GetConnectedController(channel))
		{
			switch (controller->get_data_format())
			{
			case WPAD_FMT_CORE:
			case WPAD_FMT_CORE_ACC:
			case WPAD_FMT_CORE_ACC_DPD:
			{
				status->fmt = controller->get_data_format();
				controller->WPADRead(&status->u.core);
				break;
			}
			case kDataFormat_FREESTYLE:
			case kDataFormat_FREESTYLE_ACC:
			case kDataFormat_FREESTYLE_ACC_DPD:
			{
				status->fmt = controller->get_data_format();
				controller->WPADRead(&status->u.fs);
				break;
			}
			case kDataFormat_CLASSIC:
			case kDataFormat_CLASSIC_ACC:
			case kDataFormat_CLASSIC_ACC_DPD:
			{
				status->fmt = controller->get_data_format();
				controller->WPADRead(&status->u.cl);
				break;
			}
			case kDataFormat_MPLS:
			{
				status->fmt = controller->get_data_format();
				controller->WPADRead(&status->u.mp);
				break;
			}
			default:
				debugBreakpoint();
			}
		}
		else
		{
			status->u.core.err = WPAD_ERR_NO_CONTROLLER;
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_KPADSetBtnRepeat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	float delaySec = hCPU->fpr[1].fpr;
	float pulseSec = hCPU->fpr[2].fpr;
	cemuLog_log(LogType::InputAPI, "KPADSetBtnRepeat({}, {}, {})", channel, delaySec, pulseSec);

	if (channel < InputManager::kMaxWPADControllers)
	{
		padscore::g_padscore.controller_data[channel].btn_repeat = { (int)delaySec, (int)pulseSec };
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_KPADSetSamplingCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "KPADSetSamplingCallback({}, 0x{:x})", channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].sampling_callback;
	padscore::g_padscore.controller_data[channel].sampling_callback = callback;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

void padscoreExport_WPADControlDpd(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(command, 1);
	ppcDefineParamMPTR(callback, 2);

	cemuLog_log(LogType::InputAPI, "WPADControlDpd({}, {}, 0x{:x})", channel, command, callback);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = padscore::GetConnectedController(channel))
		{
			padscore::g_padscore.controller_data[channel].dpd_callback = callback;
			if (callback)
			{
				coreinitAsyncCallback_add(callback, 2, channel, WPAD_ERR_NONE);
			}

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADSetExtensionCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "WPADSetExtensionCallback({}, 0x{:x})", channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].extension_callback;
	padscore::g_padscore.controller_data[channel].extension_callback = callback;
	padscore::g_padscore.controller_data[channel].connection.extension = kWAPDevNotFound;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

void padscoreExport_KPADSetConnectCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "KPADSetConnectCallback({}, 0x{:x})",channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].connectCallback;
	padscore::g_padscore.controller_data[channel].connectCallback = callback;
	padscore::g_padscore.controller_data[channel].connection.known = false;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

uint64 g_kpadLastRead[InputManager::kMaxWPADControllers] = {0};
bool g_kpadIsInited = true;

sint32 _KPADRead(uint32 channel, KPADStatus_t* samplingBufs, uint32 length, betype<KPAD_ERROR>* errResult)
{

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		return 0;
	}
	
	if (g_kpadIsInited == false)
	{
		if (errResult)
			*errResult = KPAD_ERROR::NOT_INITIALIZED;

		return 0;
	}

	const auto controller = padscore::GetConnectedController(channel);
	if (!controller)
	{
		if (errResult)
			*errResult = KPAD_ERROR::NO_CONTROLLER;

		return 0;
	}
	controller->sync_data_format();

	// On console new input samples are only received every few ms and calling KPADRead(Ex) clears the internal queue regardless of length value
	// thus calling KPADRead(Ex) again too soon on the same channel will result in no data being returned
	// Games that depend on this: Affordable Space Adventures
	uint64 currentTime = coreinit::OSGetTime();
	uint64 timeDif = currentTime - g_kpadLastRead[channel];
	if(length == 0 || timeDif < coreinit::EspressoTime::ConvertNsToTimerTicks(1000000))
	{
		if (errResult)
			*errResult = KPAD_ERROR::NO_SAMPLE_DATA;
		return 0;
	}
	g_kpadLastRead[channel] = currentTime;

	memset(samplingBufs, 0x00, sizeof(KPADStatus_t));
	samplingBufs->wpadErr = WPAD_ERR_NONE;
	samplingBufs->data_format = controller->get_data_format();
	samplingBufs->devType = padscore::GetReportedDeviceType(channel, controller);
	if(!WindowSystem::InputConfigWindowHasFocus())
	{
		const auto btn_repeat = padscore::g_padscore.controller_data[channel].btn_repeat;
		controller->KPADRead(*samplingBufs, btn_repeat);
		if (padscore::g_padscore.controller_data[channel].mpls_mode == 0)
			samplingBufs->mpls = {};
		else if (controller->is_mpls_attached())
		{
			auto& processing = padscore::g_padscore.controller_data[channel].mpls_processing;
			auto& mp = samplingBufs->mpls;
			MplsProcessing::Vec angle{mp.angle.x, mp.angle.y, mp.angle.z};
			MplsProcessing::Matrix dir{{{mp.dir.X.x, mp.dir.X.y, mp.dir.X.z},
				{mp.dir.Y.x, mp.dir.Y.y, mp.dir.Y.z}, {mp.dir.Z.x, mp.dir.Z.y, mp.dir.Z.z}}};
			processing.Apply(angle, dir);
			const float dt = static_cast<float>(timeDif) / coreinit::EspressoTime::GetTimerClock();
			processing.Correct(dir, {samplingBufs->acc.x, samplingBufs->acc.y, samplingBufs->acc.z}, dt,
				samplingBufs->dpd_valid_fg == 2 && padscore::g_padscore.controller_data[channel].dpd_enabled,
				{samplingBufs->pos.x, samplingBufs->pos.y}, {samplingBufs->horizon.x, samplingBufs->horizon.y});
			mp.angle = {angle[0], angle[1], angle[2]};
			mp.dir.X = {dir[0][0], dir[0][1], dir[0][2]};
			mp.dir.Y = {dir[1][0], dir[1][1], dir[1][2]};
			mp.dir.Z = {dir[2][0], dir[2][1], dir[2][2]};
			processing.UpdateCalibration({mp.mpls.x, mp.mpls.y, mp.mpls.z}, controller->is_mpls_calibrated(), dt);
		}
	}

	if (errResult)
		*errResult = KPAD_ERROR::NONE;

	static const bool trace_motion = std::getenv("CEMU_WIIMOTE_GUEST_TRACE") != nullptr;
	static std::chrono::steady_clock::time_point next_trace[InputManager::kMaxWPADControllers]{};
	if (trace_motion && std::chrono::steady_clock::now() >= next_trace[channel])
	{
		next_trace[channel] = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
		const auto& mp = samplingBufs->mpls;
		const auto& acc = samplingBufs->acc;
		const auto mode = padscore::g_padscore.controller_data[channel].mpls_mode.value_or(255);
		cemuLog_log(LogType::Force, "KPAD guest[{}] dev={} mode={} acc=({:.5f},{:.5f},{:.5f}) gyro=({:.5f},{:.5f},{:.5f}) angle=({:.5f},{:.5f},{:.5f}) dir=({:.5f},{:.5f},{:.5f};{:.5f},{:.5f},{:.5f};{:.5f},{:.5f},{:.5f}) fs=({:.4f},{:.4f}) buttons={:#x}",
			channel, samplingBufs->devType, mode, float(acc.x), float(acc.y), float(acc.z),
			float(mp.mpls.x), float(mp.mpls.y), float(mp.mpls.z),
			float(mp.angle.x), float(mp.angle.y), float(mp.angle.z),
			float(mp.dir.X.x), float(mp.dir.X.y), float(mp.dir.X.z),
			float(mp.dir.Y.x), float(mp.dir.Y.y), float(mp.dir.Y.z),
			float(mp.dir.Z.x), float(mp.dir.Z.y), float(mp.dir.Z.z),
			float(samplingBufs->ex_status.fs.stick.x), float(samplingBufs->ex_status.fs.stick.y), uint32(samplingBufs->hold));
		const auto& correction = padscore::g_padscore.controller_data[channel].mpls_processing;
		const auto& raw = correction.last_dir;
		const auto& base = correction.revise_base;
		cemuLog_log(LogType::Force, "KPAD reference[{}] enabled=({},{},{}) weight=({:.4f},{:.4f},{:.4f}) applied=({:.4f},{:.4f},{:.4f}) dpd={} pos=({:.4f},{:.4f}) horizon=({:.4f},{:.4f}) raw=({:.5f},{:.5f},{:.5f};{:.5f},{:.5f},{:.5f};{:.5f},{:.5f},{:.5f}) base=({:.5f},{:.5f},{:.5f};{:.5f},{:.5f},{:.5f};{:.5f},{:.5f},{:.5f})",
			channel, correction.dir_revise, correction.acc_revise, correction.dpd_revise,
			correction.dir_weight, correction.acc_weight, correction.dpd_weight,
			correction.dir_level, correction.acc_level, correction.dpd_level,
			samplingBufs->dpd_valid_fg, float(samplingBufs->pos.x), float(samplingBufs->pos.y),
			float(samplingBufs->horizon.x), float(samplingBufs->horizon.y),
			raw[0][0],raw[0][1],raw[0][2],raw[1][0],raw[1][1],raw[1][2],raw[2][0],raw[2][1],raw[2][2],
			base[0][0],base[0][1],base[0][2],base[1][0],base[1][1],base[1][2],base[2][0],base[2][1],base[2][2]);
	}

	return 1;
}

void padscoreExport_KPADReadEx(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(kpadStatus, KPADStatus_t, 1);
	ppcDefineParamU32(length, 2);
	ppcDefineParamPtr(errResult, betype<KPAD_ERROR>, 3);
	cemuLog_log(LogType::InputAPI, "KPADReadEx({}, 0x{:x})", channel, length);

	sint32 samplesRead = _KPADRead(channel, kpadStatus, length, errResult);
	osLib_returnFromFunction(hCPU, samplesRead);
}

void padscoreExport_KPADRead(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(kpadStatus, KPADStatus_t, 1);
	ppcDefineParamU32(length, 2);
	cemuLog_log(LogType::InputAPI, "KPADRead({}, 0x{:x})", channel, length);

	sint32 samplesRead = _KPADRead(channel, kpadStatus, length, nullptr);
	osLib_returnFromFunction(hCPU, samplesRead);
}






#pragma endregion



namespace padscore
{
	void export_KPADEnableDPD(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamS32(channel, 0);
		cemuLog_log(LogType::InputAPI, "KPADEnableDPD({})", channel);
		cemu_assert_debug(0 <= channel && channel < InputManager::kMaxWPADControllers);

		if(const auto controller = padscore::GetConnectedController(channel))
		{
			if (controller->get_data_format() != kDataFormat_FREESTYLE && controller->get_data_format() != 0x1F)
				g_padscore.controller_data[channel].dpd_enabled = true;
		}
		

		osLib_returnFromFunction(hCPU, 0);
	}

	void export_KPADGetMplsWorkSize(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADGetMplsWorkSize()");
		osLib_returnFromFunction(hCPU, 0x5FE0);
	}

	void KPADInitEx(KPADUnifiedWpadStatus_t ring_buffer[], uint32 length)
	{
		if (g_padscore.kpad_initialized)
			return;

		for (uint32 i = 0; i < InputManager::kMaxWPADControllers; i++)
		{
			g_padscore.controller_data[i].dpd_enabled = true;


		}

		g_kpad_ringbuffer = ring_buffer;
		g_kpad_ringbuffer_length = length;
		g_padscore.kpad_initialized = true;
	}

	void export_KPADInit(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADInit()");
		KPADInitEx(nullptr, 0);
		osLib_returnFromFunction(hCPU, 0);
	}

	void export_KPADInitEx(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamMEMPTR(ring_buffer, KPADUnifiedWpadStatus_t, 0);
		ppcDefineParamU32(length, 1);
		cemuLog_log(LogType::InputAPI, "KPADInitEx(0x{:08x}, 0x{:x})", ring_buffer.GetMPTR(), length);
		KPADInitEx(ring_buffer.GetPtr(), length);
		osLib_returnFromFunction(hCPU, 0);
	}

	void WPADSetCallbackByKPAD(bool state)
	{
		g_wpad_callback_by_kpad = state;
	}

	void export_WPADSetCallbackByKPAD(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(state, 0);
		cemuLog_log(LogType::InputAPI, "WPADSetCallbackByKPAD({})", state);
		WPADSetCallbackByKPAD(state);
		osLib_returnFromFunction(hCPU, 0);
	}

	void export_KPADGetMaxControllers(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADGetMaxControllers()");
		sint32 max_controllers = g_padscore.max_controllers;
		osLib_returnFromFunction(hCPU, max_controllers);
	}

	bool WPADIsUsedCallbackByKPAD()
	{
		return g_wpad_callback_by_kpad == TRUE;
	}

	void* WPADSetSamplingCallback(sint32 channel, void* callback)
	{
		cemu_assert_debug(0 <= channel && channel < kKPADMaxControllers);

		const auto result = g_padscore.controller_data[channel].sampling_callback;
		g_padscore.controller_data[channel].sampling_callback = callback;
		return result.GetPtr();
	}

	void export_KPADSetMaxControllers(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(new_max_count, 0);
		cemuLog_log(LogType::InputAPI, "KPADSetMaxControllers({})", new_max_count);

		if (new_max_count != kKPADMaxControllers && new_max_count != kWPADMaxControllers)
		{
			debugBreakpoint();
			osLib_returnFromFunction(hCPU, -4);
			return;
		}


		const uint32 max_controllers = g_padscore.max_controllers;
		if (max_controllers == new_max_count)
		{
			osLib_returnFromFunction(hCPU, 0);
			return;
		}

		WPADSetCallbackByKPAD(FALSE);
		for (sint32 i = 0; i < kKPADMaxControllers; i++)
		{
			//WPADSetSamplingCallback(i, 0); TODO
		}

		g_padscore.max_controllers = new_max_count;
		
		WPADSetCallbackByKPAD(true);
		osLib_returnFromFunction(hCPU, new_max_count);
	}

	void export_WPADInit()
	{
		if (g_wpad_state != kWPADStateShutdown)
		{
			if (g_wpad_state == kWPADStateInitializing || g_wpad_state == kWPADStateAcquired)
				return;
		}
		g_wpad_state = kWPADStateInitializing;
	}


	enum class WPADStatus : sint32
	{
		Success = 0,
		NoController = -1,
		Busy = -2,
	};

	WPADStatus WPADIsMplsAttached(sint32 index, uint32be* attached, void* callback)
	{
		if (index < 0 || index >= InputManager::kMaxWPADControllers)
			return WPADStatus::NoController;
		auto& channel = g_padscore.controller_data[index];
		const auto controller = GetConnectedController(index);
		if (!controller)
		{
			if (attached) *attached = false;
			if (callback) PPCCoreCallback(MEMPTR(callback), index, WPADStatus::NoController);
			return WPADStatus::NoController;
		}
		if (channel.attached_query_pending)
		{
			if (callback) PPCCoreCallback(MEMPTR(callback), index, WPADStatus::Busy);
			return WPADStatus::Busy;
		}
		channel.pending_attached = attached;
		channel.pending_attached_callback = callback;
		channel.attached_query_pending = true;
		return WPADStatus::Success;
	}

	void KPADSetMplsWorkarea(void* ptr)
	{
		g_mpls_workarea = ptr; // HLE owns the processing state; retain the guest work-area contract.
		g_mpls_work_requested = ptr != nullptr;
	}

	void KPADEnableMpls(sint32 index, uint8 mode)
	{
		if (index < 0 || index >= InputManager::kMaxWPADControllers || !WpadLifecycle::ValidMode(mode))
			return;
		auto& channel = g_padscore.controller_data[index];
		if (channel.mpls_mode != mode)
		{
			channel.mpls_mode = mode;
			channel.mpls_requested = true;
			channel.mpls_config_pending = true;
		}
	}

	void KPADDisableMpls(sint32 index)
	{
		if (index < 0 || index >= InputManager::kMaxWPADControllers) return;
		auto& channel = g_padscore.controller_data[index];
		channel.mpls_mode = 0;
		channel.mpls_config_pending = true;
	}

	uint8 KPADGetMplsStatus(sint32 index)
	{
		if (index < 0 || index >= InputManager::kMaxWPADControllers) return 0;
		const auto controller = GetConnectedController(index);
		if (!controller) return 0;
		return WpadLifecycle::MotionPlusStatus(
			g_padscore.controller_data[index].mpls_mode.value_or(g_padscore.controller_data[index].mpls_requested ? 5 : 0),
			controller->is_mpls_ready(), controller->get_device_type());
	}

	void KPADSetControlMplsCallback(sint32 index, MEMPTR<void> callback)
	{
		if (index >= 0 && index < InputManager::kMaxWPADControllers)
			g_padscore.controller_data[index].mpls_callback = callback;
	}

	void KPADResetMpls(sint32 index)
	{
		if (index >= 0 && index < InputManager::kMaxWPADControllers)
			g_padscore.controller_data[index].mpls_processing = {};
	}
	void KPADSetMplsAngle(sint32 index, float x, float y, float z)
	{
		if (index >= 0 && index < InputManager::kMaxWPADControllers)
			g_padscore.controller_data[index].mpls_processing.SetAngle({x, y, z});
	}
	void KPADSetMplsDirection(sint32 index, KPADMPDir_t* dir)
	{
		if (index >= 0 && index < InputManager::kMaxWPADControllers && dir)
			g_padscore.controller_data[index].mpls_processing.SetDirection({{{dir->X.x, dir->X.y, dir->X.z},
				{dir->Y.x, dir->Y.y, dir->Y.z}, {dir->Z.x, dir->Z.y, dir->Z.z}}});
	}

	static MplsProcessing* MplsState(sint32 index)
	{
		return index >= 0 && index < InputManager::kMaxWPADControllers ?
			&g_padscore.controller_data[index].mpls_processing : nullptr;
	}
	void KPADEnableMplsDirRevise(sint32 index) { if (auto p=MplsState(index)) p->dir_revise=true; }
	void KPADDisableMplsDirRevise(sint32 index) { if (auto p=MplsState(index)) p->dir_revise=false; }
	float KPADIsEnableMplsDirRevise(sint32 index)
	{ auto p=MplsState(index); return p && p->dir_revise ? p->dir_level : -1.0f; }
	void KPADSetMplsDirReviseBase(sint32 index, KPADMPDir_t* base)
	{
		if (auto p=MplsState(index); p && base)
			p->SetReviseBase({{{base->X.x,base->X.y,base->X.z},
				{base->Y.x,base->Y.y,base->Y.z},{base->Z.x,base->Z.y,base->Z.z}}});
	}
	void KPADSetMplsDirReviseParam(sint32 index, float weight)
	{ if (auto p=MplsState(index); p && std::isfinite(weight)) p->dir_weight=std::clamp(weight,0.0f,1.0f); }
	void KPADInitMplsDirReviseParam(sint32 index) { if (auto p=MplsState(index)) p->dir_weight=0.5f; }
	void KPADGetMplsDirReviseParam(sint32 index, float32be* weight)
	{ if (auto p=MplsState(index); p && weight) *weight=p->dir_weight; }
	void KPADEnableMplsAccRevise(sint32 index) { if (auto p=MplsState(index)) p->acc_revise=true; }
	void KPADDisableMplsAccRevise(sint32 index) { if (auto p=MplsState(index)) p->acc_revise=false; }
	float KPADIsEnableMplsAccRevise(sint32 index)
	{ auto p=MplsState(index); return p && p->acc_revise ? p->acc_level : -1.0f; }
	void KPADSetMplsAccReviseParam(sint32 index, float weight, float range)
	{
		if (auto p=MplsState(index); p && std::isfinite(weight) && std::isfinite(range))
		{ p->acc_weight=std::clamp(weight,0.0f,1.0f); p->acc_range=std::clamp(range,0.0f,1.0f); }
	}
	void KPADInitMplsAccReviseParam(sint32 index)
	{ if (auto p=MplsState(index)) { p->acc_weight=0.03f; p->acc_range=0.4f; } }
	void KPADGetMplsAccReviseParam(sint32 index, float32be* weight, float32be* range)
	{ if (auto p=MplsState(index)) { if(weight) *weight=p->acc_weight; if(range) *range=p->acc_range; } }
	void KPADEnableMplsDpdRevise(sint32 index) { if (auto p=MplsState(index)) p->dpd_revise=true; }
	void KPADDisableMplsDpdRevise(sint32 index) { if (auto p=MplsState(index)) p->dpd_revise=false; }
	float KPADIsEnableMplsDpdRevise(sint32 index)
	{ auto p=MplsState(index); return p && p->dpd_revise ? p->dpd_level : -1.0f; }
	void KPADSetMplsDpdReviseParam(sint32 index, float weight)
	{ if (auto p=MplsState(index); p && std::isfinite(weight)) p->dpd_weight=std::clamp(weight,0.0f,1.0f); }
	void KPADInitMplsDpdReviseParam(sint32 index) { if (auto p=MplsState(index)) p->dpd_weight=0.05f; }
	void KPADGetMplsDpdReviseParam(sint32 index, float32be* weight)
	{ if (auto p=MplsState(index); p && weight) *weight=p->dpd_weight; }
	void KPADStartMplsCalibration(sint32 index)
	{
		if (index >= 0 && index < InputManager::kMaxWPADControllers)
			g_padscore.controller_data[index].mpls_processing.StartCalibration();
	}
	float KPADWorkMplsCalibration(sint32 index)
	{
		if (index < 0 || index >= InputManager::kMaxWPADControllers || !GetConnectedController(index)) return -1.0f;
		return g_padscore.controller_data[index].mpls_processing.CalibrationProgress();
	}
	void KPADStopMplsCalibration(sint32 index)
	{
		if (index >= 0 && index < InputManager::kMaxWPADControllers)
			g_padscore.controller_data[index].mpls_processing.calibrating = false;
	}

	struct WPADAcc
	{
		sint32be x;
		sint32be y;
		sint32be z;
	};
	void WPADGetAccGravityUnit(sint32 index, uint32 type, WPADAcc* acc)
	{
		if (index >= kKPADMaxControllers)
			return;

		// default values for wiimote usually are around 99
		acc->x = 99;
		acc->y = 99;
		acc->z = 99;
	}


#pragma endregion

	void TickFunction(PPCInterpreter_t* hCPU)
	{
		PollSync();
		for (uint32 i = 0; i < InputManager::kMaxWPADControllers; ++i)
		{
			auto& channel = g_padscore.controller_data[i];
			const auto controller = GetConnectedController(i);
			const bool live = controller != nullptr;
			const bool was_known = channel.connection.known;
			if (channel.connection.Update(live))
			{
				// SDK clears extension callbacks on link changes. Games normally
				// register them again from their connection callback.
				if (was_known) channel.extension_callback = nullptr;
				if (!live && channel.mpls_config_inflight)
				{
					channel.mpls_config_inflight = false;
					if (channel.mpls_callback) PPCCoreCallback(channel.mpls_callback, i, 1);
				}
				if (channel.connectCallback)
					PPCCoreCallback(channel.connectCallback, i, live ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER);
				cemuLog_log(LogType::InputAPI, "WPAD channel {} connection={}", i, live);
			}
			if (channel.attached_query_pending)
			{
				const auto output = channel.pending_attached;
				const auto callback = channel.pending_attached_callback;
				channel.attached_query_pending = false;
				channel.pending_attached = nullptr;
				channel.pending_attached_callback = nullptr;
				if (output) *output = controller && controller->is_mpls_attached();
				if (callback) PPCCoreCallback(callback, i, live ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER);
			}
			const auto speaker_callback = channel.speaker_callback;
			std::optional<sint32> speaker_result;
			const auto speaker_command=channel.speaker.pending;
			if(speaker_command && !live) speaker_result=-1;
			else if (speaker_command)
			{
				if(!channel.speaker_submitted)
				{
					if(const auto native=NativeRemote(i)) channel.speaker_submitted=native->control_speaker(*speaker_command,WPADGetSpeakerVolume());
					else speaker_result=0; // Logical-only remotes still have no stream sink.
				}
				if(channel.speaker_submitted)
				{
					if(const auto native=NativeRemote(i)) speaker_result=native->speaker_control_result();
					else speaker_result=-1;
				}
			}
			if (speaker_result)
			{
				channel.speaker.Complete(*speaker_result==0 && live && HasSpeaker(i));
				channel.speaker_callback = nullptr;
				channel.speaker_submitted=false;
				if (speaker_callback) PPCCoreCallback(speaker_callback, i, static_cast<uint32>(*speaker_result));
				cemuLog_log(LogType::Force, "WPAD speaker control[{}] command={} complete={} enabled={} playing={}", i, *speaker_command, *speaker_result, channel.speaker.enabled,channel.speaker.playing);
			}
			else if(!speaker_command && !live) channel.speaker.Complete(false);
			if (!live) continue;

			const uint8 status = KPADGetMplsStatus(i);
			if (channel.mpls_config_pending)
			{
				channel.mpls_config_pending = false;
				channel.mpls_config_inflight = true;
				if (channel.mpls_callback) PPCCoreCallback(channel.mpls_callback, i, 0);
			}
			if ((channel.mpls_config_inflight || channel.mpls_last_status != status) &&
				(status != 0 || channel.mpls_mode == 0 || !controller->is_mpls_attached()))
			{
				channel.mpls_config_inflight = false;
				channel.mpls_last_status = status;
				if (channel.mpls_callback)
					PPCCoreCallback(channel.mpls_callback, i,
						channel.mpls_mode.value_or(0) && !controller->is_mpls_attached() ?
						(*channel.mpls_mode == 7 ? -3 : *channel.mpls_mode == 5 ? -2 : -1) : 1);
			}
			const auto type = GetReportedDeviceType(i, controller);
			if (channel.extension_callback && channel.connection.extension != type)
			{
				const auto callback = channel.extension_callback;
				channel.connection.extension = type;
				if (type != kWAPDevCore && type != kWAPDevMPLS)
					PPCCoreCallback(callback, i, kWAPDevUnknown);
				if (channel.extension_callback == callback)
					PPCCoreCallback(callback, i, type);
				cemuLog_log(LogType::InputAPI, "WPAD channel {} extension={}", i, (uint32)type);
			}
			if (channel.sampling_callback)
				PPCCoreCallback(channel.sampling_callback, i);
		}
		osLib_returnFromFunction(hCPU, 0);
	}

	static void ResetTitleState()
	{
		// Guest pointers must not survive the module/title that owns them.
		// Ordinary HOME suspension never unloads this module.
		for (auto& channel : g_padscore.controller_data) channel = {};
		g_padscore.kpad_initialized = false;
		g_padscore.max_controllers = kWPADMaxControllers;
		g_kpad_ringbuffer = nullptr;
		g_kpad_ringbuffer_length = 0;
		g_wpad_callback_by_kpad = false;
		g_wpad_state = kWPADStateMaster;
		g_mpls_work_requested = false;
		g_mpls_workarea = nullptr;
		s_syncCallback = nullptr;
		s_syncActive = s_syncStarted = s_syncAcked = false;
	}

	void start()
	{
		OSCreateAlarm(&g_padscore.alarm);
		const uint64 start_tick = coreinit::OSGetTime();
		const uint64 period_tick = coreinit::EspressoTime::GetTimerClock() / 200; // every 5ms
		MPTR handler = PPCInterpreter_makeCallableExportDepr(TickFunction);
		OSSetPeriodicAlarm(&g_padscore.alarm, start_tick, period_tick, handler);
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "padscore";
		}

		void RPLUnmapped() override
		{
			coreinit::OSCancelAlarm(g_padscore.alarm.GetPtr());
			ResetTitleState();
			s_padscoreStaticData.Deallocate();
		}

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
				start();
		}

		void RPLMapped() override
		{
			s_padscoreStaticData.Allocate();
			ResetTitleState();
			cafeExportRegister("padscore", WPADControlSpeaker, LogType::InputAPI);
			cafeExportRegister("padscore", WPADIsSpeakerEnabled, LogType::InputAPI);
			cafeExportRegister("padscore", WPADCanSendStreamData, LogType::InputAPI);
			cafeExportRegister("padscore", WPADSendStreamData, LogType::InputAPI);
			cafeExportRegister("padscore", WPADGetSpeakerVolume, LogType::InputAPI);
			cafeExportRegister("padscore", WPADSetSpeakerVolume, LogType::InputAPI);
			cafeExportRegister("padscore", WPADEnableMotor, LogType::InputAPI);
			cafeExportRegister("padscore", WPADStartSyncDevice, LogType::InputAPI);
			cafeExportRegister("padscore", WPADCancelSyncDevice, LogType::InputAPI);
			cafeExportRegister("padscore", WPADIsBusyForSync, LogType::InputAPI);
			cafeExportRegister("padscore", WPADSetSyncDeviceCallback, LogType::InputAPI);
			cafeExportRegister("padscore", WPADDeleteControllerOrder, LogType::InputAPI);
			cafeExportRegister("padscore", WPADSaveConfig, LogType::InputAPI);
			cafeExportRegister("padscore", WENCGetEncodeData, LogType::InputAPI);
			cafeExportRegister("padscore", WPADIsMplsAttached, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetMplsWorkarea, LogType::InputAPI);
			cafeExportRegister("padscore", KPADEnableMpls, LogType::InputAPI);
			cafeExportRegister("padscore", KPADDisableMpls, LogType::InputAPI);
			cafeExportRegister("padscore", KPADGetMplsStatus, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetControlMplsCallback, LogType::InputAPI);
			cafeExportRegister("padscore", KPADResetMpls, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetMplsAngle, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetMplsDirection, LogType::InputAPI);
			cafeExportRegister("padscore", KPADEnableMplsDirRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADDisableMplsDirRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADIsEnableMplsDirRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetMplsDirReviseBase, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetMplsDirReviseParam, LogType::InputAPI);
			cafeExportRegister("padscore", KPADInitMplsDirReviseParam, LogType::InputAPI);
			cafeExportRegister("padscore", KPADGetMplsDirReviseParam, LogType::InputAPI);
			cafeExportRegister("padscore", KPADEnableMplsAccRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADDisableMplsAccRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADIsEnableMplsAccRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetMplsAccReviseParam, LogType::InputAPI);
			cafeExportRegister("padscore", KPADInitMplsAccReviseParam, LogType::InputAPI);
			cafeExportRegister("padscore", KPADGetMplsAccReviseParam, LogType::InputAPI);
			cafeExportRegister("padscore", KPADEnableMplsDpdRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADDisableMplsDpdRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADIsEnableMplsDpdRevise, LogType::InputAPI);
			cafeExportRegister("padscore", KPADSetMplsDpdReviseParam, LogType::InputAPI);
			cafeExportRegister("padscore", KPADInitMplsDpdReviseParam, LogType::InputAPI);
			cafeExportRegister("padscore", KPADGetMplsDpdReviseParam, LogType::InputAPI);
			cafeExportRegister("padscore", KPADStartMplsCalibration, LogType::InputAPI);
			cafeExportRegister("padscore", KPADWorkMplsCalibration, LogType::InputAPI);
			cafeExportRegister("padscore", KPADStopMplsCalibration, LogType::InputAPI);
			cafeExportRegister("padscore", WPADGetAccGravityUnit, LogType::InputAPI);

			// wpad
			//osLib_addFunction("padscore", "WPADInit", padscore::export_WPADInit);

			// kpad
			osLib_addFunction("padscore", "KPADSetMaxControllers", padscore::export_KPADSetMaxControllers);
			osLib_addFunction("padscore", "KPADGetMaxControllers", padscore::export_KPADGetMaxControllers);
			osLib_addFunction("padscore", "KPADEnableDPD", padscore::export_KPADEnableDPD);
			osLib_addFunction("padscore", "KPADGetMplsWorkSize", padscore::export_KPADGetMplsWorkSize);
			osLib_addFunction("padscore", "KPADInit", padscore::export_KPADInit);
			osLib_addFunction("padscore", "KPADInitEx", padscore::export_KPADInitEx);

			osLib_addFunction("padscore", "KPADSetConnectCallback", padscoreExport_KPADSetConnectCallback);
			osLib_addFunction("padscore", "KPADReadEx", padscoreExport_KPADReadEx);
			osLib_addFunction("padscore", "KPADRead", padscoreExport_KPADRead);
			osLib_addFunction("padscore", "KPADGetUnifiedWpadStatus", padscoreExport_KPADGetUnifiedWpadStatus);
			osLib_addFunction("padscore", "KPADSetSamplingCallback", padscoreExport_KPADSetSamplingCallback);
			osLib_addFunction("padscore", "KPADSetBtnRepeat", padscoreExport_KPADSetBtnRepeat);

			osLib_addFunction("padscore", "WPADGetBatteryLevel", padscoreExport_WPADGetBatteryLevel);
			osLib_addFunction("padscore", "WPADControlMotor", padscoreExport_WPADControlMotor);
			osLib_addFunction("padscore", "WPADIsMotorEnabled", padscoreExport_WPADIsMotorEnabled);
			osLib_addFunction("padscore", "WPADGetStatus", padscoreExport_WPADGetStatus);
			osLib_addFunction("padscore", "WPADProbe", padscoreExport_WPADProbe);
			osLib_addFunction("padscore", "WPADGetInfoAsync", padscoreExport_WPADGetInfoAsync);
			osLib_addFunction("padscore", "WPADGetInfo", padscoreExport_WPADGetInfo);
			osLib_addFunction("padscore", "WPADSetConnectCallback", padscoreExport_KPADSetConnectCallback);
			osLib_addFunction("padscore", "WPADSetDataFormat", padscoreExport_WPADSetDataFormat);
			osLib_addFunction("padscore", "WPADGetDataFormat", padscoreExport_WPADGetDataFormat);
			osLib_addFunction("padscore", "WPADRead", padscoreExport_WPADRead);
			osLib_addFunction("padscore", "WPADSetExtensionCallback", padscoreExport_WPADSetExtensionCallback);
			osLib_addFunction("padscore", "WPADSetSamplingCallback", padscoreExport_KPADSetSamplingCallback);
			osLib_addFunction("padscore", "WPADControlDpd", padscoreExport_WPADControlDpd);

			osLib_addFunction("padscore", "WPADSetCallbackByKPAD", padscore::export_WPADSetCallbackByKPAD);
		};


	}s_COSCoreinitModule;

	COSModule* GetModule()
	{
		return &s_COSCoreinitModule;
	}

}
