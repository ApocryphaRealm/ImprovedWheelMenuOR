#include "Wheels.h"

namespace wheels
{
	namespace
	{
		std::atomic<int> g_active{ static_cast<int>(Wheel::kEquipment) };
	}

	const char* Name(Wheel a_wheel)
	{
		return a_wheel == Wheel::kMagic ? "Magic" : "Equipment";
	}

	Wheel Active()
	{
		return static_cast<Wheel>(g_active.load());
	}

	void Favourite(menus::Menu a_menu)
	{
		const Wheel w = a_menu == menus::Menu::kMagic ? Wheel::kMagic : Wheel::kEquipment;
		logger::info("wheels: FAVOURITE pressed in the {} - toggles the highlighted entry on the {} wheel (not wired yet)",
			menus::Name(a_menu), Name(w));
	}

	void CycleEntry(Wheel a_wheel, int a_slot, int a_dir)
	{
		logger::info("wheels: CYCLE {} wheel slot {} {} (not wired yet)", Name(a_wheel), a_slot, a_dir > 0 ? "next" : "previous");
	}

	void SwitchWheel(int a_dir)
	{
		const Wheel next = Active() == Wheel::kEquipment ? Wheel::kMagic : Wheel::kEquipment;   // two wheels: either way flips
		g_active.store(static_cast<int>(next));
		logger::info("wheels: SWITCH {} -> the {} wheel is active (the radial's slots are not rewritten yet)",
			a_dir > 0 ? "right" : "left", Name(next));
	}

	void RemoveEntry(Wheel a_wheel, int a_slot)
	{
		logger::info("wheels: REMOVE the active entry of {} wheel slot {} (not wired yet)", Name(a_wheel), a_slot);
	}

	void UseNow(int a_slot)
	{
		logger::info("wheels: USE slot {} now (RB) - the radial closes on it", a_slot);
	}
}
