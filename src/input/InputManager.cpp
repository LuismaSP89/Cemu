#include "input/InputManager.h"
#include "config/ActiveSettings.h"
#include "input/ControllerFactory.h"
#include <boost/property_tree/ini_parser.hpp>
#include <pugixml.hpp>
#include "Cafe/GameProfile/GameProfile.h"
#include "util/EventService.h"

#include "input/api/Wiimote/WiimoteAutoSlots.h"
#include "input/api/Wiimote/WiimoteSpeakerTrace.h"
#include "input/WpadChannelOrder.h"
#include "config/CemuConfig.h"
#include <cstdlib>

InputManager::InputManager()
{
	const auto automatic = std::getenv("CEMU_AUTO_CONTROLLER_SLOTS");
	m_auto_configure_enabled = AutoControllerSwitch::Enabled(false, automatic && std::string_view(automatic) == "1", false);
	/*
	auto create_provider = []
	template <typename TProvider>
	()
	{
		static_assert(std::is_base_of_v<ControllerProvider, TProvider>);
		try
		{
			auto controller = std::make_shared<TProvider>();
			m_api_available[controller->api()] = controller;
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, ex.what());
		}
	}
	*/
#if HAS_KEYBOARD
	create_provider<KeyboardControllerProvider>();
#endif
#ifdef HAS_SDL
	create_provider<SDLControllerProvider>();
#endif
#if HAS_XINPUT
	create_provider<XInputControllerProvider>();
#endif
#if HAS_DIRECTINPUT
	create_provider<DirectInputControllerProvider>();
#endif
#if HAS_DSU
	create_provider<DSUControllerProvider>();
#endif
#if defined(HAS_GAMECUBE) && HAS_GAMECUBE && defined(HAS_LIBUSB)
	create_provider<GameCubeControllerProvider>();
#endif
#if HAS_WIIMOTE
	create_provider<WiimoteControllerProvider>();
#endif

	m_update_thread_shutdown.store(false);
	m_update_thread = std::thread(&InputManager::update_thread, this);
}

InputManager::~InputManager()
{
	// destructors will not invoked forever, so we manually release resources in Shutdown().
}

void InputManager::load() noexcept
{
	m_auto_configure_enabled = AutoControllerSwitch::Enabled(GetConfig().automaticControllerSetup.GetValue(),m_auto_configure_enabled,false);
	cemuLog_log(LogType::Force, "Controller automatic setup={} (host owns device assignment and profiles)", m_auto_configure_enabled.load());
	for (size_t i = 0; i < kMaxController; ++i)
	{
		try
		{
			load(i);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "can't load controller profile: {}", ex.what());
		}
	}
	if (m_auto_configure_enabled)
	{
		std::scoped_lock lock(m_auto_configure_mutex);
		const std::array<const char*,2> names{"Pro", "Wiimote"};
		for (size_t player = 1; player < kMaxController; ++player)
			for (size_t kind = 0; kind < names.size(); ++kind)
			{
				const auto path = ActiveSettings::GetConfigPath("controllerProfiles/auto-config/controller{}/{}.xml", player, names[kind]);
				if (!fs::exists(path)) continue;
				const auto restored = read_controller_profile(player, path, true);
				const auto type = kind == 0 ? EmulatedController::Pro : EmulatedController::Wiimote;
				if (restored && restored->type() == type) m_auto_slots[player].profiles[kind] = restored;
			}
	}
	m_profiles_loaded = true;
}

bool InputManager::load(size_t player_index, std::string_view filename)
{
	fs::path file_path;
	if (filename.empty())
		file_path = ActiveSettings::GetConfigPath("controllerProfiles/controller{}", player_index);
	else
		file_path = ActiveSettings::GetConfigPath("controllerProfiles/{}", filename);

	auto old_file = file_path;
	old_file.replace_extension(".txt"); // test .txt extension
	file_path.replace_extension(".xml"); // force .xml extension

	if (fs::exists(old_file) && !fs::exists(file_path))
		migrate_config(old_file);

	if (!fs::exists(file_path))
		return false;

	const auto controller = read_controller_profile(player_index, file_path);
	if (!controller) return false;
	try
	{
		set_controller(controller);
		return true;
	}
	catch (const std::exception& error)
	{
		cemuLog_log(LogType::Force, "can't load config file: {}", error.what());
		return false;
	}
}

EmulatedControllerPtr InputManager::read_controller_profile(size_t player_index, const fs::path& file_path, bool inactive)
{
	try
	{
		auto xmlData = FileStream::LoadIntoMemory(file_path);
		if (!xmlData || xmlData->empty())
			return {};
	
		pugi::xml_document doc;
		if (!doc.load_buffer(xmlData->data(), xmlData->size()))
			return {};

		const pugi::xml_node root = doc.document_element();

		const auto type_node = root.child("type");
		if (!type_node)
			return {};

		const auto emulate = EmulatedController::type_from_string(type_node.child_value());
		if (inactive)
		{
			// Do not open a cached alternative already owned by a different player.
			for (const auto source : root.select_nodes("controller"))
			{
				const auto node = source.node();
				const std::string_view api = node.child_value("api"), uuid = node.child_value("uuid");
				if (api == "Keyboard" || api == "DSUController") continue;
				for (size_t seat = 0; seat < kMaxController; ++seat)
					if (seat != player_index)
						if (const auto owner = get_controller(seat))
							for (const auto& device : owner->controllers_snapshot())
								if (device->api_name() == api && device->uuid() == uuid) return {};
			}
		}
		auto emulated_controller = ControllerFactory::CreateEmulatedController(player_index, emulate);


		if (const auto profile_name_node = root.child("profile"))
			emulated_controller->m_profile_name = profile_name_node.child_value();

		// custom settings
		emulated_controller->load(root);

		for (const auto controller_node : root.select_nodes("controller"))
		{
			const auto cnode = controller_node.node();
			const auto api_node = cnode.child("api");
			if (!api_node)
				continue;

			const auto uuid_node = cnode.child("uuid");
			if (!uuid_node)
				continue;

			const auto* display_name = cnode.child_value("display_name");

			try
			{
				const auto api = InputAPI::from_string(api_node.child_value());
				auto controller = ControllerFactory::CreateController(api, uuid_node.child_value(), display_name);
				emulated_controller->add_controller(controller);

				// load optional settings
				auto settings = controller->get_settings();
				if (const auto axis_node = cnode.child("axis"))
				{
					if (const auto value = axis_node.child("deadzone"))
						settings.axis.deadzone = ConvertString<float>(value.child_value());

					if (const auto value = axis_node.child("range"))
						settings.axis.range = ConvertString<float>(value.child_value());
				}
				if (const auto rotation_node = cnode.child("rotation"))
				{
					if (const auto value = rotation_node.child("deadzone"))
						settings.rotation.deadzone = ConvertString<float>(value.child_value());

					if (const auto value = rotation_node.child("range"))
						settings.rotation.range = ConvertString<float>(value.child_value());
				}
				if (const auto trigger_node = cnode.child("trigger"))
				{
					if (const auto value = trigger_node.child("deadzone"))
						settings.trigger.deadzone = ConvertString<float>(value.child_value());

					if (const auto value = trigger_node.child("range"))
						settings.trigger.range = ConvertString<float>(value.child_value());
				}

				if (const auto value = cnode.child("rumble"))
					settings.rumble = ConvertString<float>(value.child_value());

				if (const auto value = cnode.child("motion"))
					settings.motion = ConvertString<bool>(value.child_value());

				controller->set_settings(settings);

				// custom settings
				controller->load(cnode);
				

				// mappings
				if (const auto mappings_node = cnode.child("mappings"))
				{
					for (const auto& entry : mappings_node.select_nodes("entry"))
					{
						const auto enode = entry.node();

						const auto mapping_node = enode.child("mapping");
						if (!mapping_node)
							continue;

						const auto button_node = enode.child("button");
						if (!button_node)
							continue;

						const auto mapping = ConvertString<uint64>(mapping_node.child_value());
						const auto button = ConvertString<uint64>(button_node.child_value());

						emulated_controller->set_mapping(mapping, controller, button);
					}
				}
			}
			catch (const std::exception& ex)
			{
				cemuLog_log(LogType::Force, "can't load controller: {}", ex.what());
			}
		}

		return emulated_controller;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't load config file: {}", ex.what());
		return {};
	}
}

bool InputManager::migrate_config(const fs::path& file_path)
{
	try
	{
		auto xmlData = FileStream::LoadIntoMemory(file_path);
		if (!xmlData || xmlData->empty())
			return false;

		std::string iniDataStr((const char*)xmlData->data(), xmlData->size());

		std::stringstream iniData(iniDataStr);
		boost::property_tree::ptree m_data;
		read_ini(iniData, m_data);

		const auto emulate_string = m_data.get<std::string>("General.emulate");
		const auto api_string = m_data.get<std::string>("General.api");
		auto uuid_opt = m_data.get_optional<std::string>("General.controller");
		const auto display_name = m_data.get_optional<std::string>("General.display");

		std::string uuid;
		if (api_string == to_string(InputAPI::Keyboard))
			uuid = to_string(InputAPI::Keyboard);
		else
		{
			if (!uuid_opt)
				return false;

			uuid = uuid_opt.value();
			if (api_string == to_string(InputAPI::SDLController))
			{
				uuid += "_0";
			}
		}

		fs::path out_file = file_path;
		out_file.replace_extension(".xml");

		pugi::xml_document doc;
		auto declaration_node = doc.append_child(pugi::node_declaration);
		declaration_node.append_attribute("version") = "1.0";
		declaration_node.append_attribute("encoding") = "UTF-8";

		auto emulated_controller = doc.append_child("emulated_controller");
		emulated_controller.append_child("type").append_child(pugi::node_pcdata).set_value(emulate_string.c_str());

		bool has_keyboard = api_string == to_string(InputAPI::Keyboard);
		if (!has_keyboard) // test if only keyboard configured
		{
			auto controller = emulated_controller.append_child("controller");
			controller.append_child("api").append_child(pugi::node_pcdata).set_value(api_string.c_str());
			controller.append_child("uuid").append_child(pugi::node_pcdata).set_value(uuid.c_str());
			if (display_name.has_value() && !display_name->empty())
				controller.append_child("display_name").append_child(pugi::node_pcdata).set_value(
					display_name.value().c_str());


			controller.append_child("rumble").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.rumble").c_str());

			auto axis_node = controller.append_child("axis");
			axis_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.leftDeadzone").c_str());
			axis_node.append_child("range").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.leftRange").c_str());

			auto rotation_node = controller.append_child("rotation");
			rotation_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.rightDeadzone").c_str());
			rotation_node.append_child("range").append_child(pugi::node_pcdata).set_value(
				m_data.get<std::string>("Controller.rightRange").c_str());

			auto mappings_node = controller.append_child("mappings");
			for (int i = 1; i < 28; ++i) // test all possible mappings (max is 27 for vpad controller)
			{
				auto mapping = m_data.get_optional<std::string>(fmt::format("Controller.{}", i));
				if (!mapping || mapping->empty())
					continue;

				if (!boost::starts_with(mapping.value(), "button_"))
				{
					if (boost::starts_with(mapping.value(), "key_"))
						has_keyboard = true;

					continue;
				}

				const auto button = ConvertString<uint64>(mapping.value().substr(7), 16);

				uint64 flag_bit = 0;
				for (auto b = 0; b < 64; ++b)
				{
					if (HAS_BIT(button, b))
					{
						flag_bit = b;
						break;
					}
				}

				// fix old flag layout to new one for all kind of axis stuff
				if (flag_bit >= 24 && flag_bit <= 31)
					flag_bit += 8;
				else if (flag_bit == 32) flag_bit = kTriggerXP;
				else if (flag_bit == 33) flag_bit = kRotationXP;
				else if (flag_bit == 34) flag_bit = kRotationYP;
				else if (flag_bit == 35) flag_bit = kTriggerYP;
				else if (flag_bit == 36) flag_bit = kAxisXN;
				else if (flag_bit == 37) flag_bit = kAxisYN;
				else if (flag_bit == 38) flag_bit = kTriggerXN;
				else if (flag_bit == 39) flag_bit = kRotationXN;
				else if (flag_bit == 40) flag_bit = kRotationYN;
				else if (flag_bit == 41) flag_bit = kTriggerYN;

				// fix old api mappings
				if (api_string == to_string(InputAPI::XInput))
				{
					const std::unordered_map<uint64, uint64> xinput =
					{
						{kButton0, 12}, // XINPUT_GAMEPAD_A
						{kButton1, 13}, // XINPUT_GAMEPAD_B
						{kButton2, 14}, // XINPUT_GAMEPAD_X
						{kButton3, 15}, // XINPUT_GAMEPAD_Y

						{kButton4, 8}, // XINPUT_GAMEPAD_LEFT_SHOULDER
						{kButton5, 9}, // XINPUT_GAMEPAD_LEFT_SHOULDER

						{kButton6, 4}, // XINPUT_GAMEPAD_START
						{kButton7, 5}, // XINPUT_GAMEPAD_BACK

						{kButton8, 6}, // XINPUT_GAMEPAD_LEFT_THUMB
						{kButton9, 7}, // XINPUT_GAMEPAD_RIGHT_THUMB

						{kButton10, 0}, // XINPUT_GAMEPAD_DPAD_UP
						{kButton11, 1}, // XINPUT_GAMEPAD_DPAD_DOWN
						{kButton12, 2}, // XINPUT_GAMEPAD_DPAD_LEFT
						{kButton13, 3}, // XINPUT_GAMEPAD_DPAD_RIGHT
					};

					const auto it = xinput.find(flag_bit);
					if (it != xinput.cend())
						flag_bit = it->second;
				}
				else if (api_string == "DSU")
				{
					const std::unordered_map<uint64, uint64> dsu =
					{
						{7, kButton0}, // ButtonSelect
						{8, kButton1}, // ButtonLStick
						{9, kButton2}, // ButtonRStick
						{6, kButton3}, // ButtonStart

						{4, kButton10}, // ButtonL
						{5, kButton11}, // ButtonR

						{0, kButton14}, // ButtonA
						{1, kButton13}, // ButtonB
						{2, kButton15}, // ButtonX
						{3, kButton12}, // ButtonY
					};

					const auto it = dsu.find(flag_bit);
					if (it != dsu.cend())
						flag_bit = it->second;
				}


				auto entry_node = mappings_node.append_child("entry");
				entry_node.append_child("mapping").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", i).c_str());
				entry_node.append_child("button").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", flag_bit).c_str());
			}
		}

		if (has_keyboard)
		{
			auto controller = emulated_controller.append_child("controller");
			controller.append_child("api").append_child(pugi::node_pcdata).set_value("Keyboard");
			controller.append_child("uuid").append_child(pugi::node_pcdata).set_value("Keyboard");

			auto mappings_node = controller.append_child("mappings");
			for (int i = 1; i < 28; ++i) // test all possible mappings (max is 27 for vpad controller)
			{
				auto mapping = m_data.get_optional<std::string>(fmt::format("Controller.{}", i));
				if (!mapping || mapping->empty())
					continue;

				if (!boost::starts_with(mapping.value(), "key_"))
					continue;

				const auto button = ConvertString<uint64>(mapping.value().substr(4));

				auto entry_node = mappings_node.append_child("entry");
				entry_node.append_child("mapping").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", i).c_str());
				entry_node.append_child("button").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", button).c_str());
			}
		}

		std::ofstream write_file(out_file, std::ios::out | std::ios::trunc);
		if (write_file.is_open())
		{
			doc.save(write_file);
			return true;
		}
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't migrate config file {}: {}", file_path.string(), ex.what());
	}

	return false;
}

void InputManager::save() noexcept
{
	for (size_t i = 0; i < kMaxController; ++i)
	{
		try
		{
			save(i);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "can't save controller profile: {}", ex.what());
		}
	}
}

bool InputManager::save(size_t player_index, std::string_view filename)
{
	std::unique_lock profileLock(m_auto_profile_mutex, std::defer_lock);
	if (m_auto_configure_enabled) profileLock.lock();
	// dont overwrite files if set by gameprofile
	if (m_is_gameprofile_set[player_index])
		return true;

	auto emulated_controller = get_controller(player_index);
	if (!emulated_controller)
		return false;
	std::optional<std::shared_lock<std::shared_mutex>> controllerLock;
	if (m_auto_configure_enabled) controllerLock.emplace(emulated_controller->m_mutex);

	fs::path file_path = ActiveSettings::GetConfigPath("controllerProfiles");
	fs::create_directories(file_path);

	const auto is_default_file = filename.empty();
	if (is_default_file)
		file_path /= fmt::format("controller{}", player_index);
	else
		file_path /= _utf8ToPath(filename);

	file_path.replace_extension(".xml"); // force .xml extension

	pugi::xml_document doc;
	auto declaration_node = doc.append_child(pugi::node_declaration);
	declaration_node.append_attribute("version") = "1.0";
	declaration_node.append_attribute("encoding") = "UTF-8";

	auto emulated_controller_node = doc.append_child("emulated_controller");
	emulated_controller_node.append_child("type").append_child(pugi::node_pcdata).set_value(std::string{
		emulated_controller->type_string()
	}.c_str());

	if(!is_default_file)
		emulated_controller->m_profile_name = std::string{filename};

	if (emulated_controller->has_profile_name())
		emulated_controller_node.append_child("profile").append_child(pugi::node_pcdata).set_value(
			emulated_controller->get_profile_name().c_str());

	// custom settings
	emulated_controller->save(emulated_controller_node);

	for (const auto& controller : emulated_controller->get_controllers())
	{
		auto controller_node = emulated_controller_node.append_child("controller");

		// general
		controller_node.append_child("api").append_child(pugi::node_pcdata).set_value(std::string{
			controller->api_name()
		}.c_str());
		controller_node.append_child("uuid").append_child(pugi::node_pcdata).set_value(controller->uuid().c_str());
		controller_node.append_child("display_name").append_child(pugi::node_pcdata).set_value(
			controller->display_name().c_str());

		// settings
		const auto& settings = controller->get_settings();

		if (controller->has_motion())
			controller_node.append_child("motion").append_child(pugi::node_pcdata).set_value(
				fmt::format("{}", settings.motion).c_str());

		if (controller->has_rumble())
			controller_node.append_child("rumble").append_child(pugi::node_pcdata).set_value(
				fmt::format("{}", settings.rumble).c_str());

		auto axis_node = controller_node.append_child("axis");
		axis_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.axis.deadzone).c_str());
		axis_node.append_child("range").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.axis.range).c_str());

		auto rotation_node = controller_node.append_child("rotation");
		rotation_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.rotation.deadzone).c_str());
		rotation_node.append_child("range").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.rotation.range).c_str());

		auto trigger_node = controller_node.append_child("trigger");
		trigger_node.append_child("deadzone").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.trigger.deadzone).c_str());
		trigger_node.append_child("range").append_child(pugi::node_pcdata).set_value(
			fmt::format("{}", settings.trigger.range).c_str());

		// custom settings
		controller->save(controller_node);

		// mappings for current controller
		auto mappings_node = controller_node.append_child("mappings");
		for (const auto& mapping : emulated_controller->m_mappings)
		{
			if (!mapping.second.controller.expired() && *controller == *mapping.second.controller.lock())
			{
				auto entry_node = mappings_node.append_child("entry");
				entry_node.append_child("mapping").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", mapping.first).c_str());
				entry_node.append_child("button").append_child(pugi::node_pcdata).set_value(
					fmt::format("{}", mapping.second.button).c_str());
			}
		}
	}
	if (controllerLock) controllerLock->unlock();
	FileStream* fs = FileStream::createFile2(file_path);
	if (!fs)
		return false;
	std::stringstream xmlData;
	doc.save(xmlData);
	std::string xmlStr = xmlData.str();
	fs->writeData(xmlStr.data(), xmlStr.size());
	delete fs;
	if (m_auto_configure_enabled && player_index > 0 && player_index < kMaxController &&
		(emulated_controller->type() == EmulatedController::Pro || emulated_controller->type() == EmulatedController::Wiimote))
	{
		const auto sidecar = ActiveSettings::GetConfigPath("controllerProfiles/auto-config/controller{}/{}.xml",
			player_index, emulated_controller->type() == EmulatedController::Pro ? "Pro" : "Wiimote");
		fs::create_directories(sidecar.parent_path());
		std::unique_ptr<FileStream> alternative(FileStream::createFile2(sidecar));
		if (!alternative) return false;
		alternative->writeData(xmlStr.data(), xmlStr.size());
	}
	return true;
}

bool InputManager::is_gameprofile_set(size_t player_index) const
{
	return m_is_gameprofile_set[player_index];
}

EmulatedControllerPtr InputManager::set_controller(EmulatedControllerPtr controller)
{
	auto prev_controller = delete_controller(controller->player_index());

	// assign controllers to new emulated controller if empty
	if (prev_controller && controller->get_controllers().empty())
	{
		for (const auto& c : prev_controller->get_controllers())
		{
			controller->add_controller(c);
		}
	}

	// try to connect all controllers
	/*for (auto& c : controller->get_controllers())
	{
		c->connect();
	}*/

	std::scoped_lock lock(m_mutex);
	switch (controller->type())
	{
	case EmulatedController::Type::VPAD:
		for (auto& pad : m_vpad)
		{
			if (!pad)
			{
				pad.swap(controller);
				return prev_controller;
			}
		}

		break;

	default:
		for (auto& pad : m_wpad)
		{
			if (!pad)
			{
				pad.swap(controller);
				return prev_controller;
			}
		}

		break;
	}
	
	cemu_assert_debug(false);
	return prev_controller;
}

EmulatedControllerPtr InputManager::set_controller(size_t player_index, EmulatedController::Type type)
{
	try
	{
		auto emulated_controller = ControllerFactory::CreateEmulatedController(player_index, type);
		set_controller(emulated_controller);
		return emulated_controller;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Unable to set controller type {} on player index {}: {}", type, player_index, ex.what());
	}

	return {};
}

EmulatedControllerPtr InputManager::set_controller(size_t player_index, EmulatedController::Type type,
                                                   const std::shared_ptr<ControllerBase>& controller)
{
	auto result = set_controller(player_index, type);
	if (result)
		result->add_controller(controller);

	return result;
}

EmulatedControllerPtr InputManager::get_controller(size_t player_index) const
{
	std::shared_lock lock(m_mutex);
	for (const auto& pad : m_vpad)
	{
		if (pad && pad->player_index() == player_index)
			return pad;
	}

	for (const auto& pad : m_wpad)
	{
		if (pad && pad->player_index() == player_index)
			return pad;
	}

	return {};
}

EmulatedControllerPtr InputManager::delete_controller(size_t player_index, bool delete_profile)
{
	std::scoped_lock lock(m_mutex);
	for (auto& controller : m_vpad)
	{
		auto result = controller;
		if (result && result->player_index() == player_index)
		{
			controller = {};

			if(delete_profile)
			{
				std::error_code ec{};
				fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.xml", player_index), ec);
				fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.txt", player_index), ec);
			}

			return result;
		}
	}

	for (auto& controller : m_wpad)
	{
		auto result = controller;
		if (result && result->player_index() == player_index)
		{
			controller = {};

			if(delete_profile)
			{
				std::error_code ec{};
				fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.xml", player_index), ec);
				fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.txt", player_index), ec);
			}

			return result;
		}
	}

	return {};
}


std::shared_ptr<VPADController> InputManager::get_vpad_controller(size_t index) const
{
	if (index >= m_vpad.size())
		return {};

	std::shared_lock lock(m_mutex);
	return std::dynamic_pointer_cast<VPADController>(m_vpad[index]);
}

std::shared_ptr<WPADController> InputManager::get_wpad_controller(size_t index) const
{
	if (index >= m_wpad.size())
		return {};

	std::shared_lock lock(m_mutex);
	if (m_wpad[index])
	{
		const auto player = m_wpad[index]->player_index();
		if (m_auto_configure_enabled && player < kMaxController && m_auto_managed[player].load() && !m_auto_present[player].load()) return {};
	}
	return std::dynamic_pointer_cast<WPADController>(m_wpad[index]);
}

std::pair<size_t, size_t> InputManager::get_controller_count() const
{
	std::shared_lock lock(m_mutex);
	const size_t vpad = std::count_if(m_vpad.cbegin(), m_vpad.cend(), [](const auto& v) { return v != nullptr; });
	const size_t wpad = std::count_if(m_wpad.cbegin(), m_wpad.cend(), [](const auto& v) { return v != nullptr; });
	return std::make_pair(vpad, wpad);
}



void InputManager::on_device_changed()
{
	std::shared_lock lock(m_mutex);

	for (auto& pad : m_vpad)
	{
		if (pad)
			pad->connect();
	}

	for (auto& pad : m_wpad)
	{
		if (pad)
			pad->connect();
	}
	lock.unlock();

	EventService::instance().signal<Events::ControllerChanged>();
}

ControllerProviderPtr InputManager::get_api_provider(InputAPI::Type api) const
{
	if(!m_api_available[api].empty())
		return *(m_api_available[api].begin());
	
	cemu_assert_debug(false);
	return {};
}

ControllerProviderPtr InputManager::get_api_provider(InputAPI::Type api, const ControllerProviderSettings& settings)
{
	for(const auto& p : m_api_available[api])
	{
		if(*p == settings)
		{
			return p;
		}
	}

	const auto result = ControllerFactory::CreateControllerProvider(api, settings);
	m_api_available[api].emplace_back(result);
	return result;
}

void InputManager::apply_game_profile()
{
	const auto& profiles = g_current_game_profile->GetControllerProfile();
	for (int i = 0; i < kMaxController; ++i)
	{
		if (profiles[i] && !profiles[i]->empty())
		{
			if (load(i, profiles[i].value()))
			{
				m_is_gameprofile_set[i] = true;
				if (const auto controller = get_controller(i))
				{
					if (!controller->has_profile_name())
						controller->m_profile_name = profiles[i].value();
				}
			}
		}
	}
}

std::vector<std::string> InputManager::get_profiles()
{
	const auto path = ActiveSettings::GetConfigPath("controllerProfiles");
	if (!exists(path))
		return {};

	std::set<std::string> tmp;
	for (const auto& entry : fs::directory_iterator(path))
	{
		const auto& p = entry.path();
		if (p.has_extension() && (p.extension() == ".xml" || p.extension() == ".txt"))
		{
			auto stem = _pathToUtf8(p.filename().stem());
			if (is_valid_profilename(stem))
			{
				tmp.emplace(stem);
			}
		}
	}

	std::vector<std::string> result;
	result.reserve(tmp.size());
	result.insert(result.end(), tmp.begin(), tmp.end());
	return result;
}

bool InputManager::is_valid_profilename(const std::string& name)
{
	if (!IsValidFilename(name))
		return false;

	// dont allow default profile names
	for (size_t i = 0; i < kMaxController; i++)
	{
		if (name == fmt::format("controller{}", i))
			return false;
	}

	return true;
}

glm::ivec2 InputManager::get_mouse_position(bool pad_window) const
{
	if (pad_window)
	{
		std::shared_lock lock(m_pad_mouse.m_mutex);
		return m_pad_mouse.position;
	}
	else
	{
		std::shared_lock lock(m_main_mouse.m_mutex);
		return m_main_mouse.position;
	}
}

std::optional<glm::ivec2> InputManager::get_left_down_mouse_info(bool* is_pad)
{
	if (is_pad)
		*is_pad = false;

	{
		std::unique_lock lock(m_main_mouse.m_mutex);
		if (std::exchange(m_main_mouse.left_down_toggle, false))
			return m_main_mouse.position;

		if (m_main_mouse.left_down)
			return m_main_mouse.position;
	}

	{
		std::unique_lock lock(m_main_touch.m_mutex);
		if (std::exchange(m_main_touch.left_down_toggle, false))
			return m_main_touch.position;

		if (m_main_touch.left_down)
			return m_main_touch.position;
	}

	if (is_pad)
		*is_pad = true;

	{
		std::unique_lock lock(m_pad_mouse.m_mutex);
		if (std::exchange(m_pad_mouse.left_down_toggle, false))
			return m_pad_mouse.position;

		if (m_pad_mouse.left_down)
			return m_pad_mouse.position;
	}

	{
		std::unique_lock lock(m_pad_touch.m_mutex);
		if (std::exchange(m_pad_touch.left_down_toggle, false))
			return m_pad_touch.position;

		if (m_pad_touch.left_down)
			return m_pad_touch.position;
	}

	return {};
}

std::optional<glm::ivec2> InputManager::get_right_down_mouse_info(bool* is_pad)
{
	if (is_pad)
		*is_pad = false;

	{
		std::unique_lock lock(m_main_mouse.m_mutex);
		if (std::exchange(m_main_mouse.right_down_toggle, false))
			return m_main_mouse.position;

		if (m_main_mouse.right_down)
			return m_main_mouse.position;
	}

	{
		std::unique_lock lock(m_main_touch.m_mutex);
		if (std::exchange(m_main_touch.right_down_toggle, false))
			return m_main_touch.position;

		if (m_main_touch.right_down)
			return m_main_touch.position;
	}

	if (is_pad)
		*is_pad = true;

	{
		std::unique_lock lock(m_pad_mouse.m_mutex);
		if (std::exchange(m_pad_mouse.right_down_toggle, false))
			return m_pad_mouse.position;

		if (m_pad_mouse.right_down)
			return m_pad_mouse.position;
	}

	{
		std::unique_lock lock(m_pad_touch.m_mutex);
		if (std::exchange(m_pad_touch.right_down_toggle, false))
			return m_pad_touch.position;

		if (m_pad_touch.right_down)
			return m_pad_touch.position;
	}

	return {};
}

void InputManager::auto_configure_controllers()
{
	if (!m_auto_configure_enabled || !m_profiles_loaded || false) return;
	for (size_t player = 1; player < kMaxController; ++player) auto_configure_slot(player);
}

void InputManager::auto_configure_slot(size_t player)
{
	if (!m_auto_configure_enabled || !m_profiles_loaded || false) return;
	if (player == 0 || player >= kMaxController) return;
	auto& managed = m_auto_managed[player];
	auto& present = m_auto_present[player];
	const auto gamepad = get_controller(0);
	const auto current = get_controller(player);
	if (!gamepad || gamepad->type() != EmulatedController::VPAD ||
		(current && current->type() != EmulatedController::Pro && current->type() != EmulatedController::Wiimote))
	{
		managed = false;
		return;
	}

	std::scoped_lock autoLock(m_auto_configure_mutex);
	auto& alternatives = m_auto_slots[player].profiles;
	auto& handoff = m_auto_slots[player].handoff;
	std::vector<std::shared_ptr<ControllerBase>> reserved;
	for (size_t seat = 0; seat < kMaxController; ++seat)
		if (seat != player)
			if (const auto pad = get_controller(seat))
				for (const auto& source : pad->controllers_snapshot()) reserved.push_back(source);
	for (size_t seat = 1; seat < kMaxController; ++seat)
		if (seat != player)
			for (const auto& pending : m_auto_slots[seat].profiles)
				if (pending && reinterpret_cast<std::uintptr_t>(pending.get()) == m_auto_slots[seat].handoff.pending)
					for (const auto& source : pending->controllers_snapshot()) reserved.push_back(source);
	const auto same = [](const auto& a, const auto& b) { return a->api() == b->api() && a->uuid() == b->uuid(); };
	const auto isReserved = [&](const auto& source) {
		return std::any_of(reserved.begin(), reserved.end(), [&](const auto& item) { return same(source, item); });
	};
	const auto isPhysical = [](const auto& source, EmulatedController::Type type) {
		if (type == EmulatedController::Wiimote) return source->api() == InputAPI::Wiimote;
		return source->api() == InputAPI::SDLController || source->api() == InputAPI::XInput ||
			source->api() == InputAPI::DirectInput || source->api() == InputAPI::GameCube;
	};
	const auto eligible = [&](const auto& pad) {
		if (!pad) return false;
		const auto sources = pad->controllers_snapshot();
		bool physical = false;
		for (const auto& source : sources)
			if (isPhysical(source, pad->type()))
			{
				if (isReserved(source)) return false;
				physical = true;
			}
		return physical;
	};
	const auto connected = [&](const auto& pad) {
		if (!eligible(pad)) return false;
		bool live = false;
		for (const auto& source : pad->controllers_snapshot())
			if (isPhysical(source, pad->type()))
			{
				if (!source->is_connected()) source->connect();
				live |= source->is_connected();
			}
		return live;
	};
	if (current && eligible(current))
		alternatives[current->type() == EmulatedController::Pro ? 0 : 1] = current;

	if (current && connected(current))
	{
		managed = true;
		present = true;
		handoff.Reset();
		return;
	}

	// Discover unassigned gamepads through SDL only. Other APIs may describe the
	// same physical device, so mixed-API configurations use their existing profile.
	const bool mixedApis = std::any_of(reserved.begin(), reserved.end(), [](const auto& source) {
		return source->api() == InputAPI::XInput || source->api() == InputAPI::DirectInput;
	});
	const std::array<InputAPI::Type,2> apis{InputAPI::SDLController, InputAPI::Wiimote};
	for (size_t kind = 0; kind < apis.size(); ++kind)
	{
		auto& alternate = alternatives[kind];
		if (connected(alternate) || (kind == 0 && mixedApis) || !is_api_available(apis[kind])) continue;
		for (const auto& source : get_api_provider(apis[kind])->get_controllers())
		{
			if (!source || isReserved(source)) continue;
			if (alternate)
			{
				const auto existing = alternate->controllers_snapshot();
				if (std::any_of(existing.begin(), existing.end(), [&](const auto& item) { return same(source,item); })) continue;
			}
			auto physical = source;
			EmulatedControllerPtr remembered;
			for (const auto& slot : m_auto_slots)
				if (const auto& previous = slot.profiles[kind])
					for (const auto& oldSource : previous->controllers_snapshot())
						if (same(oldSource,source)) { remembered = previous; physical = oldSource; }
			if (!physical->connect() || !physical->is_connected()) continue;
			auto candidate = ControllerFactory::CreateEmulatedController(player, kind == 0 ? EmulatedController::Pro : EmulatedController::Wiimote);
			candidate->add_controller(physical);
			if (remembered)
			{
				pugi::xml_document settings;
				auto root = settings.append_child("emulated_controller");
				remembered->save(root);
				candidate->load(root);
				std::shared_lock mappingLock(remembered->m_mutex);
				for (const auto& [id,mapping] : remembered->m_mappings)
					if (const auto mapped = mapping.controller.lock(); mapped && same(mapped,physical))
						candidate->set_mapping(id,physical,mapping.button);
			}
			else physical->set_rumble(1.0f);
			if (!candidate->set_default_mapping(physical) && candidate->m_mappings.empty()) continue;
			alternate = std::move(candidate);
			break;
		}
	}

	using AutoControllerSwitch::Kind;
	const auto currentKind = !current ? Kind::None : current->type() == EmulatedController::Pro ? Kind::Pro : Kind::Wiimote;
	const bool proLive = connected(alternatives[0]), remoteLive = connected(alternatives[1]);
	const auto kind = AutoControllerSwitch::Select(currentKind, proLive, remoteLive);
	const auto next = kind == Kind::None ? EmulatedControllerPtr{} : alternatives[kind == Kind::Pro ? 0 : 1];
	managed = true;
	if (!next)
	{
		present = false;
		handoff.Reset();
		return;
	}
	if (current == next)
	{
		present = true;
		handoff.Reset();
		return;
	}

	const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
	const bool firstHandoff = handoff.pending != reinterpret_cast<std::uintptr_t>(next.get());
	present = false;
	if (firstHandoff && current && !save(player))
	{
		handoff.Reset();
		cemuLog_log(LogType::Force, "Controller {} auto-switch held: previous profile could not be saved", player + 1);
		return;
	}
	// Expose a real disconnect before changing controller type so games can
	// clear their old callbacks/data format rather than receiving stale Pro input.
	if (!handoff.Ready(reinterpret_cast<std::uintptr_t>(current.get()), reinterpret_cast<std::uintptr_t>(next.get()), now)) return;

	{
		std::scoped_lock lock(m_mutex);
		const auto findPlayer = [&](size_t player) -> EmulatedControllerPtr {
			for (const auto& pad : m_vpad) if (pad && pad->player_index() == player) return pad;
			for (const auto& pad : m_wpad) if (pad && pad->player_index() == player) return pad;

			return {};
		};
		if (findPlayer(0) != gamepad || findPlayer(player) != current) { handoff.Reset(); return; }
		const auto nextSources = next->controllers_snapshot();
		for (size_t seat = 0; seat < kMaxController; ++seat)
			if (seat != player)
				if (const auto pad = findPlayer(seat))
					for (const auto& source : pad->controllers_snapshot())
						for (const auto& nextSource : nextSources)
							if (isPhysical(nextSource,next->type()) && same(source,nextSource)) { handoff.Reset(); return; }
		auto slot = std::find_if(m_wpad.begin(), m_wpad.end(), [&](const auto& pad) { return pad && pad->player_index() == player; });
		if (slot == m_wpad.end()) slot = std::find(m_wpad.begin(), m_wpad.end(), nullptr);
		if (slot == m_wpad.end()) return;
		*slot = std::dynamic_pointer_cast<WPADController>(next);
		present = true;
		handoff.Reset();
	}
	if (current) current->stop_rumble();
	save(player);
	cemuLog_log(LogType::Force, "Controller {} auto-switch: {} active; GamePad controller 1 unchanged", player + 1, next->type_string());
	EventService::instance().signal<Events::ControllerChanged>();
}

void InputManager::auto_configure_wiimotes()
{
#if HAS_WIIMOTE
	if (m_auto_configure_enabled) return;
	if (!m_profiles_loaded || false || !is_api_available(InputAPI::Wiimote)) return;
	const auto provider = std::static_pointer_cast<WiimoteControllerProvider>(get_api_provider(InputAPI::Wiimote));
	// do not start the Wii Remote workers just to look for hotplugged remotes: only once a profile uses the
	// Wiimote API (the workers already run) or the dedicated USB adapter is enabled
	if (!provider->is_active()) return;
	for (const auto& native : provider->get_controllers())
	{
		if (!native) continue;
		auto inspect = [&](std::array<bool, kMaxController>& occupied) {
			bool bound = false;
			auto visit = [&](const EmulatedControllerPtr& pad) {
				if (!pad) return;
				if (pad->player_index() < occupied.size()) occupied[pad->player_index()] = true;
				std::shared_lock controller_lock(pad->m_mutex);
				for (const auto& source : pad->m_controllers)
					if (source->api() == native->api() && source->uuid() == native->uuid()) bound = true;
			};
			for (const auto& pad : m_vpad) visit(pad);
			for (const auto& pad : m_wpad) visit(pad);

			return bound;
		};
		std::array<bool, kMaxController> occupied{};
		{
			std::shared_lock lock(m_mutex);
			if (inspect(occupied)) continue;
		}
		if (!native->is_connected()) continue;
		const auto slot = WiimoteAutoSlots::FirstFree(occupied);
		if (!slot) continue;
		auto candidate = ControllerFactory::CreateEmulatedController(*slot, EmulatedController::Wiimote);
		native->set_rumble(1.0f);
		candidate->add_controller(native);
		if (!candidate->set_default_mapping(native)) continue;
		{
			std::scoped_lock lock(m_mutex);
			occupied.fill(false);
			if (inspect(occupied) || occupied[*slot]) continue;
			const auto free = std::find(m_wpad.begin(), m_wpad.end(), nullptr);
			if (free == m_wpad.end()) continue;
			*free = std::dynamic_pointer_cast<WPADController>(candidate);
		}
		cemuLog_log(LogType::Force, "Wiimote auto-configuration: live remote assigned to controller position {} without replacing existing profiles", *slot + 1);
		EventService::instance().signal<Events::ControllerChanged>();
	}
#endif
}

void InputManager::reconcile_wpad_channels()
{
	std::scoped_lock lock(m_mutex);
	const auto live = [&](const auto& pad) {
		if (!pad) return false;
		const auto player = pad->player_index();
		if (m_auto_configure_enabled && player < kMaxController && m_auto_managed[player] && !m_auto_present[player]) return false;
		const auto wpad = std::dynamic_pointer_cast<WPADController>(pad);
		return wpad && wpad->is_guest_connected();
	};
	if (m_auto_configure_enabled) input::ReconcileWpadChannels(m_wpad, m_live_wpad_channels, live);
	for (size_t channel = 0; channel < m_wpad.size(); ++channel)
		if (m_wpad[channel]) m_wpad[channel]->set_player_led_index(channel);
}

void InputManager::update_thread()
{
	SetThreadName("Input_update");
	auto next_discovery = std::chrono::steady_clock::now();
	auto next_automatic = next_discovery;
	while (!m_update_thread_shutdown.load(std::memory_order::relaxed))
	{
		const auto now = std::chrono::steady_clock::now();
		if (m_wiimote_pairing_requested.exchange(false, std::memory_order_acq_rel))
		{
			// Native HOME and game WPAD sync requests use the same host worker
			// as ordinary hotplug, never the guest's callback/fiber stack.
			try
			{
				if (is_api_available(InputAPI::Wiimote))
					std::static_pointer_cast<WiimoteControllerProvider>(get_api_provider(InputAPI::Wiimote))->request_discovery();
			}
			catch (const std::exception& error) { cemuLog_log(LogType::Force, "Wiimote pairing request failed: {}", error.what()); }
			next_automatic = next_discovery = now;
		}
		if (now >= next_automatic)
		{
			try { auto_configure_controllers(); reconcile_wpad_channels(); }
			catch (const std::exception& error) { cemuLog_log(LogType::Force, "Automatic controller configuration failed: {}", error.what()); }
			next_automatic = now + std::chrono::milliseconds(100);
		}
		if (now >= next_discovery)
		{
			try { auto_configure_wiimotes(); }
			catch (const std::exception& error) { cemuLog_log(LogType::Force, "Wiimote auto-configuration failed: {}", error.what()); }
			next_discovery = now + std::chrono::milliseconds(500);
		}
		std::shared_lock lock(m_mutex);

		for (auto& pad : m_vpad)
		{
			if (pad)
				pad->update();
		}

		for (auto& pad : m_wpad)
		{
			if (pad)
				pad->update();
		}
		lock.unlock();

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::this_thread::yield();
	}
}

void InputManager::Shutdown()
{
    m_update_thread_shutdown = true;

    if (m_update_thread.joinable())
    {
        m_update_thread.join();
    }

    for (auto& pad : m_vpad) pad.reset();
	for (auto& pad : m_wpad) pad.reset();


	for (auto& providers : m_api_available)
	{
#if HAS_WIIMOTE
		for (const auto& provider : providers) if (const auto wiimote=std::dynamic_pointer_cast<WiimoteControllerProvider>(provider)) wiimote->shutdown_usb_bluetooth();
#endif
		providers.clear();
	}
	WiimoteSpeakerTrace::FlushAll();
}
