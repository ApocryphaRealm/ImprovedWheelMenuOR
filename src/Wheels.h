#pragma once

#include "Menus.h"

// ============================================================================================================
// The two wheels (Equipment, Magic) x eight slots x up to uEntriesPerSlot entries - the owner's rulings of
// 2026-09-26 and 2026-09-29 (PLAN.md). The controls (Pad.cpp) call these; every call is on the game thread.
//
// EQUIPMENT: the active entry of a slot is the item carrying the game's quick key (Inventory.h), so the game's
// own radial, its "use" on close and the save all keep working untouched; the other entries are ours, saved per
// character in OBSE\Plugins\ImprovedWheelMenu\<character>.txt.
//   Assign (the game's A in the inventory's panel) is watched, not replaced: before the game handles the press the
//   pointed slot's item is taken off the key, then the result is read back -
//     the same item came back   -> it was pressed again on the same item: REMOVED from the slot
//     another item arrived      -> ADDED as a new entry (the old ones stay; it becomes the active one)
//     an inactive entry arrived -> REMOVED (pressing A on anything already in the slot takes it out)
//     nothing happened          -> the item goes back on its key
// MAGIC: where the game keeps a spell's quick key is not found yet (RESEARCH.md) - the Magic wheel is logged only.
// ============================================================================================================

namespace wheels
{
	enum class Wheel { kEquipment, kMagic };

	const char* Name(Wheel a_wheel);
	Wheel Active();                            // the wheel the HUD radial shows

	void Favourite(menus::Menu a_menu);        // Y in the inventory / magic menu: the highlighted item or spell on/off its wheel
	void AssignPressed(menus::Menu a_menu, int a_slot);   // the game's A with a menu's panel showing (a_slot = pointed)
	void Tick(bool a_assignHeld);              // every controller read: settles a pending assign
	void CycleEntry(Wheel a_wheel, int a_slot, int a_dir);   // step the active entry of a slot (D-pad L/R in a menu panel, LT/RT on the radial)
	void SwitchWheel(int a_dir);               // D-pad L/R on the HUD radial
	void RemoveEntry(Wheel a_wheel, int a_slot);   // LB on the HUD radial
	void UseNow(int a_slot);                   // RB on the HUD radial (the radial then closes on this slot)
}
