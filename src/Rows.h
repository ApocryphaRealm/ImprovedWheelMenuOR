#pragma once

// ============================================================================================================
// The rows of the inventory and magic menu lists - which one is highlighted, and what each shows. Read in game
// 2026-09-29 with the UE4SS probe:
//   WBP_OriginalMenu_InventoryEntry_C   Properties : OriginalInventoryMenuItemProperties
//                                         { Name (FText), Icon (texture), bIsFavorite, form (UTESForm*: m_formID) ... }
//   WBP_ModernMenu_MagicEntry_C         Properties : LegacyMagicMenuItemProperties
//                                         { Name (FText), Icon (texture), bIsFavorite, InventoryIndex ... }  - no form
// Both fire BP_OnItemSelectionChanged(bool) through ProcessEvent as the highlight moves (watched with pe::Watch).
// A magic row carries no form, so its spell is the player's spell of that name. Game thread only.
// ============================================================================================================

namespace rows
{
	void Tick();                            // finds and watches the row classes as the menus first load them

	std::uint32_t HighlightedItem();        // inventory: the highlighted row's item formID (0 = none)

	// a container or barter list's highlighted row (the same row widget): the player's own item, if it is one
	std::uint32_t HighlightedPlayerItemInContainer();
	std::uint32_t HighlightedSpell();       // magic menu: the highlighted row's spell formID (0 = none / not a spell)

	UE::UObject* ItemIcon(std::uint32_t a_formID);    // the icon a row showed for it this session (nullptr = not seen)
	UE::UObject* SpellIcon(std::uint32_t a_formID);

	std::uint32_t SpellByName(const std::string& a_name);   // the player's spell of that name (0 = none)
	std::string   SpellName(std::uint32_t a_formID);

	// the favourites column (Favourites.cpp): every live inventory row now, the key its name reads (a string-table key,
	// else the text) and the item it shows; true from TakeInventoryRowsChanged when a row fired an event since the last ask
	std::vector<UE::UObject*> LiveInventoryRows();
	std::string               InventoryRowKey(UE::UObject* a_row);
	std::uint32_t             InventoryRowForm(UE::UObject* a_row);
	bool                      TakeInventoryRowsChanged();
	UE::UClass*               InventoryRowClass();

	std::string Status();                   // for the self-check
}
