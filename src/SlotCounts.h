#pragma once

// ============================================================================================================
// Each inventory-wheel slot's count against the most it holds, "3/5" (the owner, 2026-09-30: "a counter on the slot
// showing how many items it holds and how many it allows in total ... so the player can see how many spaces are left").
// Eight labels of the game's own text prefab, laid over the game's wheel picture while the inventory wheel is up (the
// inventory's panel, or the HUD radial on the Equipment wheel), each on its slot circle. MagicWheel.cpp, which already
// finds and measures that picture, drives it.
// ============================================================================================================

namespace slotcounts
{
	void Show(UE::UObject* a_gameWheelImage);   // the game's wheel picture on screen now (the inventory wheel)
	void Hide();
}
