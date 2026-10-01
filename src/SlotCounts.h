#pragma once

// ============================================================================================================
// Each inventory-wheel slot's counter: which of its items the slot shows now, out of the items LT / RT can reach, "2/3"
// (the owner, 2026-10-01: "the numbers for which item I'm selecting isn't changing whether I'm on one out of five, two out
// of five" - it had shown the count against the cap, "3/5", since 2026-09-30). An empty slot shows nothing.
// Eight labels of the game's own text prefab, laid over the game's wheel picture while the inventory wheel is up (the
// inventory's panel, or the HUD radial on the Equipment wheel), each on its slot circle. MagicWheel.cpp, which already
// finds and measures that picture, drives it.
// ============================================================================================================

namespace slotcounts
{
	void Show(UE::UObject* a_gameWheelImage);   // the game's wheel picture on screen now (the inventory wheel)
	void Hide();
}
