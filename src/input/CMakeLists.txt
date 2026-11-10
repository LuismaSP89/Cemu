add_library(CemuInput
	InputManager.cpp
	InputManager.h
	ControllerFactory.cpp
	ControllerFactory.h
	api/ControllerState.h
	api/Controller.cpp
	api/Controller.h
	api/ControllerState.cpp
	api/InputAPI.h
	api/ControllerProvider.h
	api/DSU/DSUController.h
	api/DSU/DSUControllerProvider.cpp
	api/DSU/DSUController.cpp
	api/DSU/DSUControllerProvider.h
	api/DSU/DSUMessages.h
	api/DSU/DSUMessages.cpp
	api/Keyboard/KeyboardControllerProvider.h
	api/Keyboard/KeyboardControllerProvider.cpp
	api/Keyboard/KeyboardController.cpp
	api/Keyboard/KeyboardController.h
	emulated/ProController.cpp
	emulated/EmulatedController.h
	emulated/EmulatedController.cpp
	emulated/ProController.h
	emulated/WPADController.cpp
	emulated/WPADController.h
	emulated/WiimoteController.h
	emulated/VPADController.cpp
	emulated/WiimoteController.cpp
	emulated/VPADController.h
	emulated/ClassicController.cpp
	emulated/ClassicController.h
	emulated/ButtonRepeatPulse.h
	AutoControllerSwitch.h
	ControllerBattery.h
	WpadChannelOrder.h
)

set_property(TARGET CemuInput PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

cemu_use_precompiled_header(CemuInput)

if (ENABLE_LIBUSB)
	target_sources(CemuInput PRIVATE
		api/GameCube/GameCubeController.cpp
		api/GameCube/GameCubeControllerProvider.h
		api/GameCube/GameCubeControllerProvider.cpp
		api/GameCube/GameCubeController.h
	)
endif()

if(WIN32)
	# XInput
	target_sources(CemuInput PRIVATE
		api/XInput/XInputControllerProvider.cpp
		api/XInput/XInputControllerProvider.h
		api/XInput/XInputController.cpp
		api/XInput/XInputController.h
	)

	# DirectInput
	target_sources(CemuInput PRIVATE
		api/DirectInput/DirectInputControllerProvider.cpp
		api/DirectInput/DirectInputController.h
		api/DirectInput/DirectInputControllerProvider.h
		api/DirectInput/DirectInputController.cpp
	)
endif()

if (SUPPORTS_WIIMOTE)
	target_compile_definitions(CemuInput PUBLIC SUPPORTS_WIIMOTE)
	target_sources(CemuInput PRIVATE
			api/Wiimote/WiimoteControllerProvider.h
			api/Wiimote/WiimoteControllerProvider.cpp
			api/Wiimote/WiimoteMessages.h
			api/Wiimote/NativeWiimoteController.h
			api/Wiimote/NativeWiimoteController.cpp
			api/Wiimote/WiimoteDevice.h
			api/Wiimote/MotionPlusFactoryCalibration.h
			api/Wiimote/MotionPlusZeroCalibration.h
			api/Wiimote/WiimoteAudioWriteQueue.h
			api/Wiimote/WiimoteAutoSlots.h
			api/Wiimote/WiimoteBattery.h
			api/Wiimote/WiimoteIrReference.h
			api/Wiimote/WiimoteKpadMotion.h
			api/Wiimote/WiimoteLifecycle.h
			api/Wiimote/WiimoteOutputGate.h
			api/Wiimote/WiimoteOutputSchedule.h
			api/Wiimote/WiimoteOutputWait.h
			api/Wiimote/WiimotePointerFilter.h
			api/Wiimote/WiimoteProbeThrottle.h
			api/Wiimote/WiimoteReportRecovery.h
			api/Wiimote/WiimoteSpeaker.h
			api/Wiimote/WiimoteSpeakerCompatibility.h
			api/Wiimote/WiimoteSpeakerProtocol.h
			api/Wiimote/WiimoteSpeakerTrace.h
			api/Wiimote/WiimoteStateCache.h
	)
	if (ENABLE_HIDAPI)
		target_sources(CemuInput PRIVATE
				api/Wiimote/hidapi/HidapiWiimote.cpp
				api/Wiimote/hidapi/HidapiWiimote.h)
		if (WIN32)
			target_sources(CemuInput PRIVATE api/Wiimote/hidapi/WindowsSpeakerWriter.h)
		endif ()
	endif ()
	if (WIN32 AND ENABLE_LIBUSB)
		# Bluetooth passthrough through a dedicated USB adapter (WinUSB via libusb)
		target_sources(CemuInput PRIVATE
			api/Wiimote/usb/UsbBluetoothHost.cpp
			api/Wiimote/usb/UsbBluetoothHost.h
			api/Wiimote/usb/UsbBluetoothAdapter.h
			api/Wiimote/usb/UsbBluetoothBonds.h
			api/Wiimote/usb/UsbBluetoothPcm.h
			api/Wiimote/usb/UsbBluetoothProtocol.h
			api/Wiimote/usb/UsbBluetoothTrace.h)
		find_package(libusb MODULE REQUIRED)
		target_link_libraries(CemuInput PRIVATE libusb::libusb Crypt32)
	endif ()


	if (ENABLE_BLUEZ)
		target_sources(CemuInput PRIVATE
				api/Wiimote/l2cap/L2CapWiimote.cpp
				api/Wiimote/l2cap/L2CapWiimote.h)
	endif()
endif ()


if(ENABLE_SDL)
	target_sources(CemuInput PRIVATE
		api/SDL/SDLController.cpp
		api/SDL/SDLControllerProvider.cpp
		api/SDL/SDLController.h
		api/SDL/SDLControllerProvider.h
	)
	target_link_libraries(CemuInput PRIVATE	SDL3::SDL3)
endif()

target_include_directories(CemuInput PUBLIC "../")

target_link_libraries(CemuInput PRIVATE
	CemuCommon
	CemuGui
)

if (ENABLE_BLUEZ)
	target_link_libraries(CemuInput PRIVATE bluez::bluez)
endif ()
