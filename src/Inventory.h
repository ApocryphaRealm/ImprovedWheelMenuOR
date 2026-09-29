#pragma once

// ============================================================================================================
// The game's own item quick keys, read and moved. An inventory item is on quick key N when one of its extra data
// lists carries ExtraQuickKey (type 0x55) with keyID N at +0x18 (read in game 2026-09-29 with TestBench re.quickkeys;
// the UI shows slot N+1). The game keeps ONE item per key, and the save stores it - so the active entry of each
// Equipment slot is simply the item carrying that key, and the wheel's other entries are ours (Wheels.cpp).
//
// Every call here walks or changes the player's inventory: game thread only (the controller read is on it).
// ============================================================================================================

namespace inventory
{
	constexpr int kSlots = 8;

	// formID of the item carrying each key (0 = none)
	using KeyMap = std::array<std::uint32_t, kSlots>;
	KeyMap Keys();

	bool Has(std::uint32_t a_formID);                 // the player carries at least one
	std::string NameOf(std::uint32_t a_formID);
	std::uint32_t FindByName(const std::string& a_name);   // a carried item whose full name (a "LOC_FN_" key, or a custom name) matches

	void ClearKey(std::uint32_t a_formID);            // takes every quick key off that item
	void ClearSlot(int a_key);                        // takes quick key N off whichever item has it
	bool SetKey(std::uint32_t a_formID, int a_key);   // puts quick key N on that item (and off any other item)

	std::string PlayerName();                         // the character the wheels are saved under
}
