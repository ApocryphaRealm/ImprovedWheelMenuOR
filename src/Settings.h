#pragma once

// ImprovedWheelMenu.ini beside the plugin. Read once at load; the compiled defaults are the shipped INI's values
// (memory: log-level-defaults-to-trace - every INI ships at info, uLogLevel=2, and the compiled default matches).

namespace settings
{
	struct Values
	{
		int  logLevel = 2;        // [Log] uLogLevel: 0 trace, 1 debug, 2 info, 3 warn, 4 error
		int  entriesPerSlot = 5;  // [Wheel] uEntriesPerSlot: how many entries one slot of a wheel holds (the owner: five by default)
		bool centreRestSnap = true;   // [Wheel] bCentreRestSnap: the right stick back at rest points at no slot
		int  restSnapMs = 150;        // [Wheel] uRestSnapMs: how long at rest before the pointer lets go
		bool ammoWheel = true;        // [AmmoWheel] bEnabled
		int  ammoButton = 0x0008;     // [AmmoWheel] uButton: XInput button mask (8 = D-pad right), with a bow held
		int  ammoScalePercent = 100;  // [AmmoWheel] uScalePercent
	};

	// Loads <plugin folder>\ImprovedWheelMenu.ini; missing file or keys keep the defaults, and the log says which.
	void Load();
	const Values& Get();
	std::filesystem::path PluginFolder();   // ...\OblivionRemastered\Binaries\Win64\OBSE\Plugins
}
