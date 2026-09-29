#pragma once

#include "Menus.h"

// ============================================================================================================
// The two wheels (Equipment, Magic) x eight slots x up to uEntriesPerSlot entries - the owner's rulings of
// 2026-09-26 and 2026-09-29 (PLAN.md). The controls (Pad.cpp) call these; each is called on the game thread.
// FIRST BUILD (2026-09-29, "get the controls down at least"): the actions are logged so every button can be tested
// before the model and the game's quick-key writes are wired in.
// ============================================================================================================

namespace wheels
{
	enum class Wheel { kEquipment, kMagic };

	const char* Name(Wheel a_wheel);
	Wheel Active();                            // the wheel the HUD radial shows

	void Favourite(menus::Menu a_menu);        // Y in the inventory / magic menu: the highlighted item or spell on/off its wheel
	void CycleEntry(Wheel a_wheel, int a_slot, int a_dir);   // step the active entry of a slot (D-pad L/R in a menu panel, LT/RT on the radial)
	void SwitchWheel(int a_dir);               // D-pad L/R on the HUD radial
	void RemoveEntry(Wheel a_wheel, int a_slot);   // LB on the HUD radial
	void UseNow(int a_slot);                   // RB on the HUD radial (the radial then closes on this slot)
}
