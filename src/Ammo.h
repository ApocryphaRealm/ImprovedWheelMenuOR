#pragma once

#include <Xinput.h>   // types and constants only - nothing is linked or loaded
#include <nlohmann/json.hpp>

// ============================================================================================================
// The ammo wheel (the owner, 2026-09-29 - to-do list "Improved Wheel Menu: an ammo wheel"): "the perfected wheeler ammo
// wheel that only appears when using a bow and toggling the wheel menu and favoriting arrows instead of sending them to
// the normal wheel will send them to the ammo wheel" - "It would default to D-pad right while holding a bow. And it would
// be similarly positioned as a semi-circle radial menu locked to the middle of the right side of the screen."
// Built from that description; Wheeler Refined's own ammo wheel is GPL-3.0-only and was not read (rule 62).
//
//   * The wheel's arrows are the Ammo wheel's eight slots (wheels::AmmoSlots): favouriting arrows (Y) puts them there,
//     never on the Equipment wheel, and arrows found on the Equipment wheel move over (Wheels.cpp).
//   * D-pad right with a bow held (in gameplay, the game's own radial closed) opens it; the game never sees that press.
//     A semi-circle of up to eight arrows along the middle of the right-hand edge, the top one first.
//   * The right stick points at an arrow; D-pad up / down step; back at rest in the middle for a moment, nothing is
//     pointed (the centre rest snap). D-pad right again or A equips the pointed arrows and closes; B closes without.
//     While it is open the right stick, the D-pad, A and B go to the wheel only.
//   * Equipping runs on the TES thread (testhread::Post - logic library 7880: the equipment path traps every other
//     thread), Actor::EquipObject with the whole stack and no lock (logic library: EquipObject's last argument).
// ============================================================================================================

namespace ammo
{
	// the pad read (game thread): a_raw / a_pressed are the buttons as read, a_out what the game will see (the buttons
	// the wheel used are taken out of it, and a_pad's right stick is zeroed while the wheel is open). True when the wheel
	// took this read - the rest of the gameplay rules then leave it alone.
	bool Rewrite(XINPUT_GAMEPAD& a_pad, WORD a_raw, WORD a_pressed, WORD& a_out, bool a_gameplay, bool a_radialOpen);
	bool Open();
	nlohmann::json State();
}
