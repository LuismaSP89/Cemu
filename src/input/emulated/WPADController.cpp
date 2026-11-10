#include <api/Controller.h>
#include "input/emulated/WPADController.h"
#include "input/emulated/ButtonRepeatPulse.h"



#include <algorithm>

#include "input/api/Wiimote/NativeWiimoteController.h"
#include "input/api/Wiimote/WiimoteControllerProvider.h"
#include "input/emulated/ClassicController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/WiimoteController.h"

#include <cstring>

WPADController::WPADController(size_t player_index, WPADDataFormat data_format)
	: EmulatedController(player_index), m_data_format(data_format)
{
}

bool WPADController::is_guest_connected() const
{
	std::shared_lock lock(m_mutex);
	bool has_native = false;
	for (const auto& controller : m_controllers)
	{
		if (controller->api() != InputAPI::Wiimote)
			continue;
		has_native = true;
		if (controller->is_connected())
			return true;
	}
	// A virtual Wiimote is intentionally connected by its configured profile.
	// A real Wiimote profile must not turn an absent remote into a guest device.
	return !has_native && !m_controllers.empty();
}

bool WPADController::is_mpls_ready() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		const auto native = std::dynamic_pointer_cast<NativeWiimoteController>(controller);
		if (native)
			return native->is_connected() && native->get_motion_plus_data().has_value();
	}
	lock.unlock();
	const auto device = get_device_type();
	return device == kWAPDevMPLS || device == kWAPDevMPLSFreeStyle || device == kWAPDevMPLSClassic;
}

bool WPADController::is_mpls_calibrated() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		const auto native = std::dynamic_pointer_cast<NativeWiimoteController>(controller);
		if (native)
		{
			const auto sample = native->get_motion_plus_data();
			return sample && sample->zero_calibration.ready && native->get_kpad_motion().ready;
		}
	}
	return true;
}

static bool IsDataFormatCompatible(WPADDataFormat format, WPADDeviceType device_type)
{
	switch (device_type)
	{
	case kWAPDevCore:
		return format == kDataFormat_CORE || format == kDataFormat_CORE_ACC || format == kDataFormat_CORE_ACC_DPD || format == kDataFormat_CORE_ACC_DPD_FULL;
	case kWAPDevFreestyle:
	case kWAPDevMPLSFreeStyle:
		return format == kDataFormat_FREESTYLE || format == kDataFormat_FREESTYLE_ACC || format == kDataFormat_FREESTYLE_ACC_DPD;
	case kWAPDevClassic:
	case kWAPDevMPLSClassic:
		return format == kDataFormat_CLASSIC || format == kDataFormat_CLASSIC_ACC || format == kDataFormat_CLASSIC_ACC_DPD;
	case kWAPDevMPLS:
		return format == kDataFormat_MPLS;
	case kWAPDevURCC:
		return format == kDataFormat_URCC;
	default:
		return true;
	}
}

void WPADController::set_data_format(WPADDataFormat data_format)
{
	m_data_format = data_format;
	m_data_format_user_set = true;
	m_last_data_format_device_type = get_device_type();
}

void WPADController::sync_data_format()
{
	const auto device_type = get_device_type();
	const auto desired_format = get_default_data_format();

	if (m_data_format_user_set)
	{
		if (IsDataFormatCompatible(m_data_format, device_type))
			m_last_data_format_device_type = device_type;
		return;
	}

	if (m_last_data_format_device_type != device_type || m_data_format != desired_format)
	{
		m_data_format = desired_format;
		m_last_data_format_device_type = device_type;
	}
}

WPADDataFormat WPADController::get_default_data_format() const
{
	switch (get_device_type())
	{
	case kWAPDevCore:
		return kDataFormat_CORE_ACC_DPD;
	case kWAPDevFreestyle:
		return kDataFormat_FREESTYLE_ACC;
	case kWAPDevClassic:
		return kDataFormat_CLASSIC;
	case kWAPDevMPLS:
		return kDataFormat_MPLS;
	case kWAPDevMPLSFreeStyle:
		return kDataFormat_FREESTYLE_ACC_DPD;
	case kWAPDevMPLSClassic:
		return kDataFormat_CLASSIC_ACC_DPD;
	case kWAPDevURCC:
		return kDataFormat_URCC;
	default:
		return kDataFormat_CORE;
	}
}

uint32 WPADController::get_emulated_button_flag(WPADDataFormat format, uint32 id) const
{
	switch(format)
	{
	case kDataFormat_CORE:
	case kDataFormat_CORE_ACC:
	case kDataFormat_CORE_ACC_DPD:
	case kDataFormat_CORE_ACC_DPD_FULL:
	case kDataFormat_FREESTYLE:
	case kDataFormat_FREESTYLE_ACC:
	case kDataFormat_FREESTYLE_ACC_DPD:
	case kDataFormat_MPLS:
		return WiimoteController::s_get_emulated_button_flag(id);
	case kDataFormat_CLASSIC:
	case kDataFormat_CLASSIC_ACC:
	case kDataFormat_CLASSIC_ACC_DPD:
		return ClassicController::s_get_emulated_button_flag(id);
	
	case kDataFormat_TRAIN: break;
	case kDataFormat_GUITAR: break;
	case kDataFormat_BALANCE_CHECKER: break;
	case kDataFormat_DRUM: break;
	
	case kDataFormat_TAIKO: break;
	case kDataFormat_URCC:
		return ProController::s_get_emulated_button_flag(id);

	}

	return 0;
}

void WPADController::WPADRead(WPADStatus_t* status)
{
	sync_data_format();
	controllers_update_states();
	uint32 button = 0;
	for (uint32 i = 1; i < get_highest_mapping_id(); ++i)
	{
		if (is_mapping_down(i))
		{
			const uint32 value = get_emulated_button_flag(m_data_format, i);
			button |= value;
		}
	}

	m_homebutton_down |= is_home_down();

	// todo fill position api from wiimote

	switch (m_data_format)
	{
	case kDataFormat_CORE:
	case kDataFormat_CORE_ACC:
	case kDataFormat_CORE_ACC_DPD:
	case kDataFormat_CORE_ACC_DPD_FULL:
	{
		memset(status, 0x00, sizeof(*status));
		status->button = button;
		break;
	}

	case kDataFormat_FREESTYLE:
	case kDataFormat_FREESTYLE_ACC:
	case kDataFormat_FREESTYLE_ACC_DPD:
	{
		WPADFSStatus_t* ex_status = (WPADFSStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->button = button;

		auto axis = get_axis();
		axis *= 127.0f;
		ex_status->fsStickX = (sint8)axis.x;
		ex_status->fsStickY = (sint8)axis.y;
		break;
	}

	case kDataFormat_CLASSIC:
	case kDataFormat_CLASSIC_ACC:
	case kDataFormat_CLASSIC_ACC_DPD:
	case kDataFormat_GUITAR:
	case kDataFormat_DRUM:
	case kDataFormat_TAIKO:
	{
		WPADCLStatus_t* ex_status = (WPADCLStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->clButton = button;
		
		auto axis = get_axis();
		axis *= 2048.0f;
		ex_status->clLStickX = (uint16)axis.x;
		ex_status->clLStickY = (uint16)axis.y;

		auto rotation = get_rotation();
		rotation *= 2048.0f;
		ex_status->clRStickX = (uint16)rotation.x;
		ex_status->clRStickY = (uint16)rotation.y;
		break;
	}
	case kDataFormat_TRAIN:
	{
		WPADTRStatus_t* ex_status = (WPADTRStatus_t*)status;
		// TODO
		break;
	}
	case kDataFormat_BALANCE_CHECKER:
	{
		WPADBLStatus_t* ex_status = (WPADBLStatus_t*)status;
		// TODO
		break;
	}
	case kDataFormat_MPLS:
	{
		WPADMPStatus_t* ex_status = (WPADMPStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->button = button;
		ex_status->stat = is_mpls_attached() ? 1 : 0;
		static uint32 s_mplsLogCounter[4]{};

		for (const auto& controller : m_controllers)
		{
			if (controller->api() != InputAPI::Wiimote)
				continue;

			auto* wiimote = static_cast<NativeWiimoteController*>(controller.get());
			const auto motion_plus = wiimote->get_motion_plus_data();
			if (!motion_plus)
				continue;

			// Use the validated delta, including zero output during startup calibration.
			const sint32 roll = static_cast<sint32>(std::round(motion_plus->orientation.x));
			const sint32 pitch = static_cast<sint32>(std::round(motion_plus->orientation.y));
			const sint32 yaw = static_cast<sint32>(std::round(motion_plus->orientation.z));

			ex_status->pitch = static_cast<uint16>(std::clamp<sint32>(pitch + 0x1F7F, 0, 0xFFFF));
			ex_status->yaw = static_cast<uint16>(std::clamp<sint32>(yaw + 0x1F7F, 0, 0xFFFF));
			ex_status->roll = static_cast<uint16>(std::clamp<sint32>(roll + 0x1F7F, 0, 0xFFFF));

			const size_t log_index = std::min<size_t>(m_player_index, 3);
			if ((s_mplsLogCounter[log_index]++ % 20) == 0)
			{
				cemuLog_log(
					LogType::InputAPI,
					"WPAD MPLS[{}] raw=({},{},{}) bias=({},{},{}) centered=({},{},{}) out=({},{},{}) slow=({},{},{})",
					m_player_index,
					motion_plus->raw.x, motion_plus->raw.y, motion_plus->raw.z,
					motion_plus->zero_calibration.bias[0], motion_plus->zero_calibration.bias[1], motion_plus->zero_calibration.bias[2],
					roll, pitch, yaw,
					ex_status->roll.value(), ex_status->pitch.value(), ex_status->yaw.value(),
					motion_plus->slow_roll, motion_plus->slow_pitch, motion_plus->slow_yaw);
			}
			break;
		}
		break;
	}
	case kDataFormat_URCC:
	{
		WPADUCStatus_t* ex_status = (WPADUCStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->ucButton = button;

		ex_status->cable = TRUE;
		ex_status->charge = TRUE;

		auto axis = get_axis();
		axis *= 2048.0f;
		ex_status->ucLStickX = (uint16)axis.x;
		ex_status->ucLStickY = (uint16)axis.y;

		auto rotation = get_rotation();
		rotation *= 2048.0f;
		ex_status->ucRStickX = (uint16)rotation.x;
		ex_status->ucRStickY = (uint16)rotation.y;

		break;
	}
	default:
		cemu_assert(false);
	}

	status->dev = get_device_type();
	status->err = WPAD_ERR_NONE;
}

void WPADController::KPADRead(KPADStatus_t& status, const BtnRepeat& repeat)
{
	std::optional<WiimoteKpadMotion::Sample> native_pose;
	sync_data_format();
	uint32be* hold, *release, *trigger;
	switch (type())
	{
	case Pro:
		hold = &status.ex_status.uc.hold;
		release = &status.ex_status.uc.release;
		trigger = &status.ex_status.uc.trig;
		break;
	case Classic:
		hold = &status.ex_status.cl.hold;
		release = &status.ex_status.cl.release;
		trigger = &status.ex_status.cl.trig;
		break;
	default:
		hold = &status.hold;
		release = &status.release;
		trigger = &status.trig;
	}

	controllers_update_states();
	for (uint32 i = 1; i < get_highest_mapping_id(); ++i)
	{
		if (is_mapping_down(i))
		{
			const uint32 value = get_emulated_button_flag(m_data_format, i);
			*hold |= value;
		}
	}

	m_homebutton_down |= is_home_down();
	
	// button repeat
	const auto now = std::chrono::steady_clock::now();
	if (input::ButtonRepeatPulse<std::chrono::steady_clock>(*hold, m_last_holdvalue,
		kWPADButtonRepeat, repeat.delay, repeat.pulse, now, m_last_hold_change, m_last_pulse))
		*hold |= kWPADButtonRepeat;

	// axis
	const auto axis = get_axis();
	const auto rotation = get_rotation();
	*release = m_last_holdvalue & ~*hold;
	//status.release = m_last_holdvalue & ~*hold;
	*trigger = ~m_last_holdvalue & *hold;
	//status.trig = ~m_last_holdvalue & *hold;
	m_last_holdvalue = *hold;

	if (is_mpls_attached())
	{
		status.mpls.dir.X.x = 1;
		status.mpls.dir.X.y = 0;
		status.mpls.dir.X.z = 0;

		status.mpls.dir.Y.x = 0;
		status.mpls.dir.Y.y = 1;
		status.mpls.dir.Y.z = 0;

		status.mpls.dir.Z.x = 0;
		status.mpls.dir.Z.y = 0;
		status.mpls.dir.Z.z = 1;
	}

	if (has_motion())
	{
		auto motion_sample = get_motion_data();

		glm::vec3 acc;
		motion_sample.getAccelerometer(&acc[0]);
		status.acc.x = acc.x;
		status.acc.y = acc.y;
		status.acc.z = acc.z;

		status.acc_value = motion_sample.getVPADAccMagnitude();
		status.acc_speed = motion_sample.getVPADAccAcceleration();

		//glm::vec2 acc_vert;
		//motion_sample.getVPADAccXY(&acc_vert[0]);
		//status.acc_vertical.x = acc_vert.x;
		//status.acc_vertical.y = acc_vert.y;

		status.accVertical.x = std::min(1.0f, std::abs(acc.x + acc.y));
		status.accVertical.y = std::min(std::max(-1.0f, -acc.z), 1.0f);

		if (is_mpls_attached())
		{
			glm::vec3 gyroChange;
			motion_sample.getKPADGyroChange(&gyroChange[0]);
			status.mpls.mpls.x = gyroChange.x;
			status.mpls.mpls.y = gyroChange.y;
			status.mpls.mpls.z = gyroChange.z;

			glm::vec3 gyroOrientation;
			motion_sample.getKPADOrientation(&gyroOrientation[0]);
			status.mpls.angle.x = gyroOrientation.x;
			status.mpls.angle.y = gyroOrientation.y;
			status.mpls.angle.z = gyroOrientation.z;

			float attitude[9];
			motion_sample.getKPADAttitudeMatrix(attitude);
			status.mpls.dir.X.x = attitude[0];
			status.mpls.dir.X.y = attitude[1];
			status.mpls.dir.X.z = attitude[2];
			status.mpls.dir.Y.x = attitude[3];
			status.mpls.dir.Y.y = attitude[4];
			status.mpls.dir.Y.z = attitude[5];
			status.mpls.dir.Z.x = attitude[6];
			status.mpls.dir.Z.y = attitude[7];
			status.mpls.dir.Z.z = attitude[8];

#if SUPPORTS_WIIMOTE
			// Match get_motion_data's first enabled source; other providers retain
			// their existing GamePad/SDL conversion and MotionSample ABI.
			{
				std::shared_lock lock(m_mutex);
				for (const auto& source : m_controllers)
				{
					if (!source->use_motion()) continue;
					if (const auto native = std::dynamic_pointer_cast<NativeWiimoteController>(source))
					{
						const auto pose = native->get_kpad_motion();
						native_pose = pose;
						status.acc = {pose.acceleration[0], pose.acceleration[1], pose.acceleration[2]};
						status.mpls.mpls = {pose.gyro[0], pose.gyro[1], pose.gyro[2]};
						status.mpls.angle = {pose.angle[0], pose.angle[1], pose.angle[2]};
						status.mpls.dir.X = {pose.dir[0][0], pose.dir[0][1], pose.dir[0][2]};
						status.mpls.dir.Y = {pose.dir[1][0], pose.dir[1][1], pose.dir[1][2]};
						status.mpls.dir.Z = {pose.dir[2][0], pose.dir[2][1], pose.dir[2][2]};
					}
					break;
				}
			}
#endif

			static uint32 s_kpadMplsLogCounter[4]{};
			const size_t log_index = std::min<size_t>(m_player_index, 3);
			const float gyro_mag = std::sqrt(
				static_cast<float>(status.mpls.mpls.x * status.mpls.mpls.x) +
				static_cast<float>(status.mpls.mpls.y * status.mpls.mpls.y) +
				static_cast<float>(status.mpls.mpls.z * status.mpls.mpls.z));
			if (gyro_mag > 0.0001f || (s_kpadMplsLogCounter[log_index]++ % 20) == 0)
			{
				cemuLog_log(
					LogType::InputAPI,
					"KPAD MPLS[{}] gyro=({:.5f},{:.5f},{:.5f}) angle=({:.5f},{:.5f},{:.5f}) acc=({:.5f},{:.5f},{:.5f})",
					static_cast<uint32>(m_player_index),
					static_cast<float>(status.mpls.mpls.x), static_cast<float>(status.mpls.mpls.y), static_cast<float>(status.mpls.mpls.z),
					static_cast<float>(status.mpls.angle.x), static_cast<float>(status.mpls.angle.y), static_cast<float>(status.mpls.angle.z),
					static_cast<float>(status.acc.x), static_cast<float>(status.acc.y), static_cast<float>(status.acc.z));
			}
		}
	}
	auto visibility = GetPositionVisibility();
	if (has_position() && visibility != PositionVisibility::NONE)
	{
		status.dpd_valid_fg = 2;

		const auto position = get_position();

		const auto pos = (position * 2.0f) - 1.0f;
		status.pos.x = pos.x;
		status.pos.y = pos.y;

		if (visibility == PositionVisibility::FULL)
		{
			const auto delta = position - get_prev_position();
			status.vec.x = delta.x;
			status.vec.y = delta.y;
			status.speed = glm::length(delta);
		}
		else
		{
			status.vec.x = 0.0f;
			status.vec.y = 0.0f;
			status.speed = 0.0f;
		}
	}
	else
		status.dpd_valid_fg = 0;
	if (native_pose)
	{
		// Held/one-dot pointer positions are usable, but not a two-dot attitude reference.
		const auto& ir = native_pose->ir;
		status.dpd_valid_fg = visibility == PositionVisibility::NONE ? 0 : ir.valid ? 2 : 1;
		if (ir.valid)
		{
			status.horizon = {ir.horizon[0], ir.horizon[1]};
			status.dist = ir.distance;
		}
	}

	switch (type())
	{
	case Wiimote:
		status.ex_status.fs.stick.x = axis.x;
		status.ex_status.fs.stick.y = axis.y;
		{
			static uint32 s_kpadFsLogCounter[4]{};
			const size_t log_index = std::min<size_t>(m_player_index, 3);
			const float stick_mag = glm::length(axis);
			if (stick_mag > 0.05f || (s_kpadFsLogCounter[log_index]++ % 20) == 0)
			{
				cemuLog_log(
					LogType::InputAPI,
					"KPAD FS[{}] devType={} format={} stick=({:.4f},{:.4f}) hold=0x{:08x}",
					static_cast<uint32>(m_player_index),
					static_cast<uint32>(get_device_type()),
					static_cast<uint32>(m_data_format),
					static_cast<float>(status.ex_status.fs.stick.x),
					static_cast<float>(status.ex_status.fs.stick.y),
					static_cast<uint32>(*hold));
			}
		}

		if(has_second_motion())
		{
			auto motion_sample = get_second_motion_data();

			glm::vec3 acc;
			motion_sample.getAccelerometer(&acc[0]);
			status.ex_status.fs.acc.x = acc.x;
			status.ex_status.fs.acc.y = acc.y;
			status.ex_status.fs.acc.z = acc.z;

			status.ex_status.fs.accValue = motion_sample.getVPADAccMagnitude();
			status.ex_status.fs.accSpeed = motion_sample.getVPADAccAcceleration();
		}

		break;
	case Pro:
		status.ex_status.uc.lstick.x = axis.x;
		status.ex_status.uc.lstick.y = axis.y;

		status.ex_status.uc.rstick.x = rotation.x;
		status.ex_status.uc.rstick.y = rotation.y;

		status.ex_status.uc.charge = FALSE;
		status.ex_status.uc.cable = TRUE;

		break;
	case Classic:
		status.ex_status.cl.lstick.x = axis.x;
		status.ex_status.cl.lstick.y = axis.y;

		status.ex_status.cl.rstick.x = rotation.x;
		status.ex_status.cl.rstick.y = rotation.y;

		if (HAS_FLAG((uint32)status.ex_status.cl.hold, kCLButton_ZL))
			status.ex_status.cl.ltrigger = 1.0f;

		if (HAS_FLAG((uint32)status.ex_status.cl.hold, kCLButton_ZR))
			status.ex_status.cl.rtrigger = 1.0f;
		break;
	default:
		cemu_assert(false);
	}

	
}
