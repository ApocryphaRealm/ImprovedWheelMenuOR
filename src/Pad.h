#pragma once

// ============================================================================================================
// The controller rules (the owner, 2026-09-29; PLAN.md "rulings"). The game reads the pad through its own
// XINPUT1_3!XInputGetState import (Steam Input answers it); that import slot is chained here the way the Oblivion
// menu framework's pad gate does it (the previous target is kept and called, so either can install first), and
// every read is rewritten on the game thread:
//
//   INVENTORY / MAGIC MENU
//     Y            -> ours: Favourite (the game never sees Y)
//     L3           -> delivered as Y (the game's sorting, which was on Y)
//     D-pad down   -> timed: released before 0.25 s = replayed as a tap (the list moves one row); held past it =
//                     a left-stick-click pulse (the game's own "Show Shortcuts" panel toggles, as L3 did)
//     D-pad L/R    -> with the panel showing: cycle the pointed slot's entry (the game never sees them)
//   GAMEPLAY
//     D-pad down   -> the game's wheel button. A TAP (under 0.25 s) keeps it held for the game, so the wheel stays
//                     open; the next press lets go of it, and the game uses the pointed slot. A hold works as before.
//     with the radial open: D-pad L/R switch wheels, LT/RT cycle the pointed slot's entries, RB uses the pointed
//     slot now, LB removes its entry - none of them reach the game.
//
// Never loads an XInput DLL (gate oblivion-plugin-never-loads-xinput): only the game's own import is used.
// ============================================================================================================

namespace pad
{
	bool Install();   // chains the import slot; call once from the lazy thread (idempotent)

	struct Status
	{
		bool          installed = false;
		std::uint64_t reads = 0;
		std::uint64_t rewritten = 0;
		std::string   previousTarget;
	};
	Status GetStatus();
}
