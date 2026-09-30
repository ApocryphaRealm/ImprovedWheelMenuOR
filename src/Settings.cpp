#include "Settings.h"

namespace settings
{
	namespace
	{
		Values g_values;

		std::filesystem::path ThisModule()
		{
			HMODULE self = nullptr;
			wchar_t buf[MAX_PATH]{};
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&ThisModule), &self) &&
				GetModuleFileNameW(self, buf, MAX_PATH)) {
				return std::filesystem::path(buf);
			}
			return {};
		}

		int ReadInt(const std::filesystem::path& a_ini, const wchar_t* a_section, const wchar_t* a_key, int a_default, int a_min, int a_max)
		{
			const int v = static_cast<int>(GetPrivateProfileIntW(a_section, a_key, a_default, a_ini.c_str()));
			return std::clamp(v, a_min, a_max);
		}
	}

	std::filesystem::path PluginFolder()
	{
		return ThisModule().parent_path();
	}

	void Load()
	{
		const auto ini = PluginFolder() / L"ImprovedWheelMenu.ini";
		Values v;
		if (std::filesystem::exists(ini)) {
			v.logLevel = ReadInt(ini, L"Log", L"uLogLevel", v.logLevel, 0, 4);
			v.entriesPerSlot = ReadInt(ini, L"Wheel", L"uEntriesPerSlot", v.entriesPerSlot, 1, 16);
			v.centreRestSnap = ReadInt(ini, L"Wheel", L"bCentreRestSnap", v.centreRestSnap, 0, 1) != 0;
			v.restSnapMs = ReadInt(ini, L"Wheel", L"uRestSnapMs", v.restSnapMs, 30, 1000);
			v.ammoWheel = ReadInt(ini, L"AmmoWheel", L"bEnabled", v.ammoWheel, 0, 1) != 0;
			v.ammoButton = ReadInt(ini, L"AmmoWheel", L"uButton", v.ammoButton, 0, 0xFFFF);
			v.ammoScalePercent = ReadInt(ini, L"AmmoWheel", L"uScalePercent", v.ammoScalePercent, 50, 200);
			g_values = v;
			logger::info("settings: {} read (uLogLevel={}, uEntriesPerSlot={}, bCentreRestSnap={}, uRestSnapMs={}, ammo wheel {} on button 0x{:04X} at {}%)",
				ini.string(), v.logLevel, v.entriesPerSlot, v.centreRestSnap, v.restSnapMs, v.ammoWheel ? "on" : "off", v.ammoButton, v.ammoScalePercent);
		} else {
			g_values = v;
			logger::warn("settings: {} not found - compiled defaults in use (uLogLevel={}, uEntriesPerSlot={})", ini.string(), v.logLevel, v.entriesPerSlot);
		}
	}

	const Values& Get()
	{
		return g_values;
	}
}
