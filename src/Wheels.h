#pragma once

#include "Menus.h"

// ============================================================================================================
// The three wheels (Equipment, Magic, Ammo) x eight slots x up to uEntriesPerSlot entries - the owner's rulings of
// 2026-09-26 and 2026-09-29 (PLAN.md). The controls (Pad.cpp) call these; every call is on the game thread.
// Slots are the game's key numbers 0-7 (the UI shows 1-8). Saved per character in
// OBSE\Plugins\ImprovedWheelMenu\<character>.txt.
//
// EQUIPMENT: the active entry of a slot is the item carrying the game's quick key (Inventory.h), so the game's
// own radial, its "use" on close and the save all keep working untouched; the other entries are ours.
//   Assign (the game's A in the inventory's panel) is watched, not replaced: before the game handles the press the
//   pointed slot's item is taken off the key, then the result is read back -
//     the same item came back   -> it was pressed again on the same item: REMOVED from the slot
//     another item arrived      -> ADDED as a new entry (the old ones stay; it becomes the active one)
//     an inactive entry arrived -> REMOVED (pressing A on anything already in the slot takes it out)
//     nothing happened          -> the item goes back on its key
// MAGIC: entirely ours - where the game keeps a spell's own quick key was never found (RESEARCH.md), so the game's
//   keys stay the Equipment wheel's. The Magic wheel is drawn by pushing its spells' icons into the wheel's view
//   model (the magic menu's panel, and the radial while Magic is the active wheel), A in the magic menu's panel is
//   ours, and choosing a Magic slot on the radial makes its spell the one the player casts.
// AMMO: entirely ours, one arrow kind a slot (the owner, 2026-09-29): Y on arrows puts them here, never on the
//   Equipment wheel, and arrows found on the Equipment wheel (an older save, or the game's own assign) move over and
//   take the game's key with them off the radial. Each wheel holds only its own kind: items, spells, arrows.
// ============================================================================================================

namespace wheels
{
	enum class Wheel { kEquipment, kMagic, kAmmo };   // kAmmo: the bow's ammo wheel (Ammo.cpp), never the radial's active wheel

	const char* Name(Wheel a_wheel);
	Wheel Active();                            // the wheel the HUD radial shows

	void Favourite(menus::Menu a_menu);        // Y in the inventory / magic menu: the highlighted item or spell on/off its wheel
	bool IsFavourite(std::uint32_t a_formID);  // on the Equipment or Ammo wheel - such an item cannot be dropped, sold or handed over

	// the Ammo wheel's eight slots, the top one first: the arrows the player carries (0 = empty, or not carried)
	std::array<std::uint32_t, 8> AmmoSlots();
	void AssignPressed(menus::Menu a_menu, int a_slot);   // A with a menu's panel showing (a_slot = pointed key, -1 none)
	void Tick(bool a_assignHeld);              // every controller read: settles a pending Equipment assign
	void CycleEntry(Wheel a_wheel, int a_slot, int a_dir);   // step the active entry of a slot (D-pad L/R in a panel, LT/RT on the radial)
	void SwitchWheel(int a_dir);               // D-pad L/R on the HUD radial
	void RemoveEntry(Wheel a_wheel, int a_slot);   // LB on the HUD radial
	void UseNow(int a_slot);                   // RB on the HUD radial (the radial then closes on this slot)
	void UseMagic(int a_slot);                 // the radial closed on a slot of the Magic wheel: that spell becomes the one cast

	// what the wheel's pictures show
	void PanelShown(menus::Menu a_menu);       // a menu's panel came up: the Magic wheel in the magic menu, Equipment in the inventory
	void PanelHidden();
	void RadialShown();
	void RadialHidden();

	std::string Status();

	// for the iwm.pad tool: each slot's entries by name, the active one marked with '*' (game thread)
	std::array<std::string, 8> Describe(Wheel a_wheel);
}
