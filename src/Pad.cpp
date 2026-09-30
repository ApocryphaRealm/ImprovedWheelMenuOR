#include "Pad.h"

#include "Ammo.h"
#include "WheelName.h"
#include "Inventory.h"
#include "MagicWheel.h"
#include "Menus.h"
#include "QuickKeys.h"
#include "Rows.h"
#include "Settings.h"
#include "Wheels.h"

#include <Xinput.h>   // types and constants only - nothing is linked or loaded

#include <deque>

namespace pad
{
	namespace
	{
		using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
		using Clock = std::chrono::steady_clock;

		constexpr auto  kHoldThreshold = 250ms;   // the Skyrim Perfected Wheeler's ToggleHoldThreshold (0.25 s)
		constexpr int   kPulseReads = 4;          // a synthetic press is held for this many reads, then let go
		constexpr BYTE  kTriggerOn = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;   // 30

		XInputGetState_t g_previous = nullptr;    // what the game's import pointed at before us (Steam's, or AMF's gate)
		std::atomic<bool> g_installed{ false };
		std::atomic<std::uint64_t> g_reads{ 0 }, g_rewritten{ 0 };
		std::string g_previousTarget;
		std::atomic<FrameCallback> g_frameCallback{ nullptr };

		// The game's own reads come from its main thread (the one OBSE loads plugins on and the one ProcessEvent runs
		// on). Another thread reads through the same import too (seen 2026-09-29: a queued A press was consumed on it and
		// ran the assign there) - it is passed straight through, untouched, so every rule and every inventory change
		// stays on the game thread.
		DWORD g_gameThread = 0;
		std::atomic<std::uint64_t> g_otherThreadReads{ 0 };

		// rule 64: presses queued by the iwm.pad TestBench tool, laid over the real pad on the game thread
		std::mutex        g_injectLock;
		std::deque<Step>  g_steps;
		bool              g_stepRunning = false;
		Clock::time_point g_stepUntil{};
		Step              g_step{};

		// applies the queued step (if any) to this read; true when a step is running
		bool Inject(XINPUT_GAMEPAD& a_pad)
		{
			std::scoped_lock l(g_injectLock);
			const auto now = Clock::now();
			if (g_stepRunning && now >= g_stepUntil) {
				g_stepRunning = false;
			}
			if (!g_stepRunning && !g_steps.empty()) {
				g_step = g_steps.front();
				g_steps.pop_front();
				g_stepRunning = true;
				g_stepUntil = now + std::chrono::milliseconds(std::max(1, g_step.ms));
			}
			if (!g_stepRunning) {
				return false;
			}
			a_pad.wButtons |= g_step.buttons;
			a_pad.bLeftTrigger = std::max(a_pad.bLeftTrigger, g_step.lt);
			a_pad.bRightTrigger = std::max(a_pad.bRightTrigger, g_step.rt);
			if (g_step.rx || g_step.ry) {
				a_pad.sThumbRX = g_step.rx;
				a_pad.sThumbRY = g_step.ry;
			}
			return true;
		}

		// ---- state, touched only inside the read (the game thread) ----
		WORD   g_prevRaw = 0;
		BYTE   g_prevLT = 0, g_prevRT = 0;
		WORD   g_prevOutButtons = 0;
		BYTE   g_prevOutLT = 0, g_prevOutRT = 0;
		DWORD  g_packetOffset = 0;
		menus::Menu g_prevMenu = menus::Menu::kNone;

		// inventory / magic menu: D-pad down timed as tap or hold
		bool              g_menuDownHeld = false;
		bool              g_menuDownFired = false;
		Clock::time_point g_menuDownAt{};

		// gameplay: the wheel button latched open by a tap
		bool              g_latched = false;
		bool              g_suppressDown = false;   // let the game see D-pad down UP until the physical button is released
		Clock::time_point g_gameDownAt{};

		int  g_pulseDown = 0;   // reads left of a replayed D-pad down tap
		bool g_swallowB = false;   // B was used to back out of the panel / radial: the game never sees it, until let go
		int  g_centreStick = 0;    // reads left with the right stick held centred (a cancelled radial must not point anywhere)
		int g_pulseLS = 0;     // reads left of a synthetic left stick click (toggles the menu's assign panel)

		bool g_closingMagic = false;   // the radial is closing on a Magic slot: the stick held centred, no slot pointed, until it has closed
		bool g_magicPanel = false;   // the magic menu's panel is up (it reports no visibility change; tracked from our own toggles)
		bool g_swallowA = false;     // A was ours (the magic menu's panel): the game never sees it, until let go
		bool g_swallowX = false;     // X (drop) on a favourite: the game never sees it, until let go

		// the centre rest snap on the HUD radial (the owner, 2026-09-29: "we'd have to add the center rest snap feature or
		// you might select items mistakenly"): once the right stick has pointed, bringing it back to rest in the middle for
		// uRestSnapMs points at no slot, so letting go of the wheel button then uses nothing
		constexpr double  kStickOn = 0.45, kStickRest = 0.25;
		bool              g_radialWasOpen = false;
		bool              g_radialAway = false;     // the stick has pointed since the last rest
		bool              g_radialResting = true;
		Clock::time_point g_radialRestSince{};

		void RestSnap(const XINPUT_GAMEPAD& a_pad, Clock::time_point a_now)
		{
			const bool open = quickkeys::RadialOpen();
			if (open != g_radialWasOpen) {
				g_radialWasOpen = open;
				g_radialAway = false;
				g_radialResting = true;
				g_radialRestSince = a_now;
			}
			if (!open || !settings::Get().centreRestSnap) {
				return;
			}
			const double m = std::hypot(a_pad.sThumbRX / 32767.0, a_pad.sThumbRY / 32767.0);
			if (m >= kStickOn) {
				g_radialAway = true;
				g_radialResting = false;
			} else if (m < kStickRest) {
				if (!g_radialResting) {
					g_radialResting = true;
					g_radialRestSince = a_now;
				}
				if (g_radialAway && a_now - g_radialRestSince >= std::chrono::milliseconds(settings::Get().restSnapMs)) {
					g_radialAway = false;
					if (quickkeys::PointedSlot() >= 1) {
						quickkeys::CancelChoice();
						logger::info("pad: the right stick came back to rest - the wheel points at no slot (centre rest snap)");
					}
				}
			}
		}

		// The wheel (HUD radial and menu panels alike) numbers its slots 1-8 as drawn, 1 at the top: key = number - 1
		// (read 2026-09-29: pointing at the game's key 0 reported 1, key 7 reported 8; 0 / -1 = none)
		int PointedKey()
		{
			const int p = quickkeys::PointedSlot();
			return p >= 1 && p <= 8 ? p - 1 : -1;
		}

		void SetMagicPanel(bool a_up)
		{
			if (g_magicPanel == a_up) {
				return;
			}
			g_magicPanel = a_up;
			if (a_up) {
				wheels::PanelShown(menus::Menu::kMagic);
			} else {
				wheels::PanelHidden();
			}
		}

		// the HUD radial is closing on a slot of the Magic wheel: the game must use nothing, the spell is ours to set
		void CloseOnMagic(int a_slot)
		{
			quickkeys::CancelChoice();
			wheels::UseMagic(a_slot);
			// the radial takes a moment to close and the stick still points meanwhile: the game pointed the slot again and
			// used ITS key there (2026-09-30 04:37:34, "the game uses slot 3" right after a Magic choice) - held off until closed
			g_closingMagic = true;
		}

		// The radial closing by the wheel button - let go after a hold, or pressed again after a tap - uses NOTHING, on either
		// wheel (the owner, 2026-09-30: "now that we have multiple items per slot will have to have the right bumper activate
		// the item and the right trigger and left trigger navigate which item they want from the slot and we'll need the
		// center rest snap because otherwise it'll just release to use instead of needing to be activated"). RB (and A) use
		// the pointed slot; the choice is kept cleared, with the stick held centred, until the radial has closed.
		void CloseOnNothing(const char* a_how)
		{
			const int pointed = PointedKey();
			quickkeys::CancelChoice();
			g_closingMagic = true;
			logger::info("pad: the wheel closed by {} - nothing used{}", a_how,
				pointed >= 0 ? std::format(" (slot {} was pointed: RB or A uses a slot)", pointed + 1) : std::string());
		}

		// the HUD radial is closing on a slot of the inventory wheel whose game key holds a spell: that slot is drawn empty
		// (MagicWheel.cpp), so it uses nothing - spells are the Magic wheel's
		void CloseOnEquipment(int a_slot)
		{
			if (magicwheel::IsSpellKey(a_slot)) {
				quickkeys::CancelChoice();
				logger::info("pad: the inventory wheel closed on slot {}, whose game key holds a spell - nothing used", a_slot + 1);
			}
		}

		// The Apocrypha Menu Framework's window is open (AMF_IsMenuOpen, AMF OR 1.0.5+). This read comes before the
		// framework's own pad gate hides the buttons from the game, so without this the D-pad still opened the wheels
		// with the framework's window up (the owner, 2026-09-30: treat it like the game's own wheel, which never opens
		// there). Looked up by name; a framework without the export (or none) leaves the rules as they were.
		bool AmfMenuOpen()
		{
			using Fn = bool (*)();
			static Fn        s_fn = nullptr;
			static ULONGLONG s_nextLook = 0;
			if (!s_fn) {
				const ULONGLONG now = GetTickCount64();
				if (now < s_nextLook) {
					return false;
				}
				s_nextLook = now + 2000;
				if (HMODULE m = ::GetModuleHandleW(L"ApocryphaMenuFramework.dll")) {
					s_fn = reinterpret_cast<Fn>(::GetProcAddress(m, "AMF_IsMenuOpen"));
					if (s_fn) {
						logger::info("pad: the menu framework reports its window - the wheels stand down while it is open");
					}
				}
				if (!s_fn) {
					return false;
				}
			}
			return s_fn();
		}

		// Tween Menu's own menu is open (TweenMenu_IsOpen, Tween Menu's export for this): the wheels - the ammo wheel above
		// all - stand down as for the framework's window (the owner, 2026-09-30: "The ammo wheel shouldn't be able to be
		// called during the tween menu"). Tween Menu draws its menu in gameplay, so no game menu reports it. Looked up by
		// name; a Tween Menu without the export (or none) leaves the rules as they were.
		bool TweenMenuOpen()
		{
			using Fn = bool (*)();
			static Fn        s_fn = nullptr;
			static ULONGLONG s_nextLook = 0;
			if (!s_fn) {
				const ULONGLONG now = GetTickCount64();
				if (now < s_nextLook) {
					return false;
				}
				s_nextLook = now + 2000;
				if (HMODULE m = ::GetModuleHandleW(L"TweenMenu.dll")) {
					s_fn = reinterpret_cast<Fn>(::GetProcAddress(m, "TweenMenu_IsOpen"));
					if (s_fn) {
						logger::info("pad: Tween Menu reports its menu - the wheels stand down while it is open");
					}
				}
				if (!s_fn) {
					return false;
				}
			}
			return s_fn();
		}

		void ResetMenuState()
		{
			g_menuDownHeld = false;
			g_menuDownFired = false;
		}

		void Rewrite(XINPUT_GAMEPAD& a_pad)
		{
			const WORD raw = a_pad.wButtons;
			const WORD pressed = raw & ~g_prevRaw;
			const WORD released = ~raw & g_prevRaw;
			const bool ltPressed = a_pad.bLeftTrigger >= kTriggerOn && g_prevLT < kTriggerOn;
			const bool rtPressed = a_pad.bRightTrigger >= kTriggerOn && g_prevRT < kTriggerOn;
			g_prevRaw = raw;
			g_prevLT = a_pad.bLeftTrigger;
			g_prevRT = a_pad.bRightTrigger;

			const auto now = Clock::now();
			if (AmfMenuOpen() || TweenMenuOpen()) {
				// the framework's window (or Tween Menu's menu) has the pad: no wheel rule runs, the read goes on untouched (its gate hides it
				// from the game); an open ammo wheel closes, a latched wheel lets go
				WORD untouched = raw;
				ammo::Rewrite(a_pad, raw, 0, untouched, false, false);
				a_pad.wButtons = raw;
				g_latched = false;
				g_suppressDown = false;
				return;
			}
			const menus::Menu menu = menus::Active();
			if (menu != g_prevMenu) {
				ResetMenuState();
				SetMagicPanel(false);
				if (menu != menus::Menu::kNone) {
					g_latched = false;       // a menu opening ends a latched wheel
					g_suppressDown = false;
				}
				g_prevMenu = menu;
			}
			WORD out = raw;

			// the ammo wheel (a bow held, its button in gameplay): while it is open it takes the read (Ammo.cpp)
			wheelname::Tick();   // "Inventory Wheel" / "Magic Wheel" over the wheel on screen
			magicwheel::Tick();  // the Magic wheel's own widget over the game's wheel; no spells on the inventory wheel
			const bool ammoTook = ammo::Rewrite(a_pad, raw, pressed, out, menu == menus::Menu::kNone && !menus::AnyOpen(), quickkeys::RadialOpen());

			if (ammoTook) {
				// the D-pad, A, B and the right stick were the ammo wheel's
			} else if (menu != menus::Menu::kNone) {
				// ---- the inventory / magic menu ----
				// the magic menu's panel, read from the widgets themselves five times a second (the owner, 2026-09-30:
				// "I switched tabs to the magic and the magic wheel doesn't appear" - the panel stayed up across the tab
				// switch, and the old toggle-tracking only learned of it from a D-pad down hold)
				if (menu == menus::Menu::kMagic) {
					static Clock::time_point s_nextLook{};
					if (now >= s_nextLook) {
						s_nextLook = now + 200ms;
						const bool up = quickkeys::AnyWheelVisible();
						if (up != g_magicPanel) {
							logger::info("pad: the magic menu's wheel panel is {}", up ? "showing - the Magic wheel" : "hidden");
						}
						SetMagicPanel(up);
					}
				}
				out &= ~XINPUT_GAMEPAD_Y;
				if (pressed & XINPUT_GAMEPAD_Y) {
					wheels::Favourite(menu);
				}
				// a favourite cannot be dropped (X drops, holding X drops the stack) - the owner, 2026-09-29
				if (menu == menus::Menu::kInventory && (pressed & XINPUT_GAMEPAD_X)) {
					const auto item = rows::HighlightedItem();
					if (wheels::IsFavourite(item)) {
						g_swallowX = true;
						logger::info("pad: X on {} - a favourite cannot be dropped", inventory::NameOf(item));
					}
				}
				out &= ~XINPUT_GAMEPAD_LEFT_THUMB;
				if (raw & XINPUT_GAMEPAD_LEFT_THUMB) {
					out |= XINPUT_GAMEPAD_Y;   // the game's sorting, moved from Y to the stick click
				}
				out &= ~XINPUT_GAMEPAD_DPAD_DOWN;
				if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) {
					g_menuDownHeld = true;
					g_menuDownFired = false;
					g_menuDownAt = now;
				}
				if (g_menuDownHeld && !g_menuDownFired && (raw & XINPUT_GAMEPAD_DPAD_DOWN) && now - g_menuDownAt >= kHoldThreshold) {
					g_menuDownFired = true;
					g_pulseLS = kPulseReads;   // the game's own Show/Hide Shortcuts
					logger::info("pad: D-pad down held in the {} - assign panel toggled", menus::Name(menu));
					}
				if (released & XINPUT_GAMEPAD_DPAD_DOWN) {
					if (g_menuDownHeld && !g_menuDownFired) {
						g_pulseDown = kPulseReads;   // a tap: the list moves one row, as before
					}
					ResetMenuState();
				}
				if (quickkeys::PanelOpen() || (menu == menus::Menu::kMagic && g_magicPanel)) {
					// B backs out of the panel (the game's own toggle, as the hold on D-pad down); the next B leaves the menu
					if (pressed & XINPUT_GAMEPAD_B) {
						g_swallowB = true;
						g_pulseLS = kPulseReads;
						SetMagicPanel(false);
						logger::info("pad: B with the assign panel showing - panel closed");
					}
					// A: in the inventory the game's Assign Item, watched (again on the same item removes it); in the magic
					// menu the Magic wheel's own, and the game never sees it
					if (pressed & XINPUT_GAMEPAD_A) {
						if (menu == menus::Menu::kMagic) {
							g_swallowA = true;
						}
						wheels::AssignPressed(menu, PointedKey());
					}
					out &= ~(XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
					const auto wheel = menu == menus::Menu::kMagic ? wheels::Wheel::kMagic : wheels::Wheel::kEquipment;
					if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) { wheels::CycleEntry(wheel, PointedKey(), -1); }
					if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) { wheels::CycleEntry(wheel, PointedKey(), +1); }
				}
			} else {
				// ---- any other menu (a container or barter list): a favourite cannot be sold or handed over. The HUD radial
				// is a Gamebryo menu too, so this checks for it first. ----
				if (menus::AnyOpen() && !quickkeys::RadialOpen() && (pressed & XINPUT_GAMEPAD_A)) {
					const auto item = rows::HighlightedPlayerItemInContainer();
					if (wheels::IsFavourite(item)) {
						g_swallowA = true;
						logger::info("pad: A on {} in a container or barter list - a favourite cannot be sold or handed over", inventory::NameOf(item));
					}
				}
				// ---- gameplay ----
				if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) {
					if (g_latched) {
						g_latched = false;
						g_suppressDown = true;   // the game sees the button go up: the wheel closes - on nothing
						if (quickkeys::RadialOpen()) {
							CloseOnNothing("the wheel button pressed again");
						}
					} else {
						g_gameDownAt = now;
					}
				}
				if (released & XINPUT_GAMEPAD_DPAD_DOWN) {
					if (g_suppressDown) {
						g_suppressDown = false;
					} else if (now - g_gameDownAt < kHoldThreshold && quickkeys::RadialOpen()) {
						g_latched = true;        // a tap: the wheel stays open until the next press
						logger::info("pad: wheel button tapped - the wheel stays open until the next press");
					} else if (quickkeys::RadialOpen()) {
						CloseOnNothing("the wheel button let go");   // a hold let go: nothing used, on either wheel
					}
				}
				out &= ~XINPUT_GAMEPAD_DPAD_DOWN;
				if (((raw & XINPUT_GAMEPAD_DPAD_DOWN) && !g_suppressDown) || g_latched) {
					out |= XINPUT_GAMEPAD_DPAD_DOWN;
				}
				if (!quickkeys::RadialOpen()) {
					RestSnap(a_pad, now);   // only notes that it closed
				}
				if (quickkeys::RadialOpen() && (pressed & XINPUT_GAMEPAD_B)) {
					// B backs out: nothing is used, the wheel closes
					quickkeys::CancelChoice();
					g_latched = false;
					if (raw & XINPUT_GAMEPAD_DPAD_DOWN) { g_suppressDown = true; }
					g_swallowB = true;
					g_centreStick = kPulseReads * 2;
					out &= ~XINPUT_GAMEPAD_DPAD_DOWN;
					logger::info("pad: B on the wheel - closed without using a slot");
				} else if (quickkeys::RadialOpen()) {
					RestSnap(a_pad, now);
					const int slot = PointedKey();
					out &= ~(XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
					if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) { wheels::SwitchWheel(-1); }
					if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) { wheels::SwitchWheel(+1); }
					if (ltPressed) { wheels::CycleEntry(wheels::Active(), slot, -1); }
					if (rtPressed) { wheels::CycleEntry(wheels::Active(), slot, +1); }
					if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) { wheels::RemoveEntry(wheels::Active(), slot); }
					// A chooses and closes, as on the game's own wheel (the owner, 2026-09-30: "I tried selecting clairvoyance
					// with pressing A like the vanilla game does and it didn't close the wheel and select the magic it only
					// selected it when I pressed the right bumper it should do both"). On the Magic wheel it is ours - the spell
					// is set and the game never sees A, so it cannot use its own key there; on the inventory wheel A stays the
					// game's, and a key holding a spell still uses nothing.
					if (pressed & XINPUT_GAMEPAD_A) {
						if (wheels::Active() == wheels::Wheel::kMagic) {
							CloseOnMagic(slot);
							g_swallowA = true;
							g_latched = false;
							if (raw & XINPUT_GAMEPAD_DPAD_DOWN) { g_suppressDown = true; }
							out &= ~(XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_A);   // let go now: the wheel closes
							logger::info("pad: A on the Magic wheel - slot {} chosen, the wheel closes", slot + 1);
						} else {
							CloseOnEquipment(slot);
						}
					}
					if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) {
						if (wheels::Active() == wheels::Wheel::kMagic) {
							CloseOnMagic(slot);
						} else {
							CloseOnEquipment(slot);
							wheels::UseNow(slot);
						}
						g_latched = false;
						if (raw & XINPUT_GAMEPAD_DPAD_DOWN) { g_suppressDown = true; }
						out &= ~XINPUT_GAMEPAD_DPAD_DOWN;   // let go now: the game closes the wheel on the pointed slot
					}
					a_pad.bLeftTrigger = 0;
					a_pad.bRightTrigger = 0;
				}
			}

			if (g_swallowB) {
				if (raw & XINPUT_GAMEPAD_B) {
					out &= ~XINPUT_GAMEPAD_B;
				} else {
					g_swallowB = false;
				}
			}
			if (g_swallowA) {
				if (raw & XINPUT_GAMEPAD_A) {
					out &= ~XINPUT_GAMEPAD_A;
				} else {
					g_swallowA = false;
				}
			}
			if (g_swallowX) {
				if (raw & XINPUT_GAMEPAD_X) {
					out &= ~XINPUT_GAMEPAD_X;
				} else {
					g_swallowX = false;
				}
			}
			if (g_closingMagic) {
				if (!quickkeys::RadialOpen()) {
					g_closingMagic = false;
				} else {
					a_pad.sThumbRX = 0;
					a_pad.sThumbRY = 0;
					if (quickkeys::PointedSlot() >= 1) {
						quickkeys::CancelChoice();
					}
				}
			}
			if (g_centreStick > 0) {
				a_pad.sThumbRX = 0;
				a_pad.sThumbRY = 0;
				--g_centreStick;
			}
			wheels::Tick((raw & XINPUT_GAMEPAD_A) != 0);

			// synthetic presses
			if (g_pulseDown > 0) { out |= XINPUT_GAMEPAD_DPAD_DOWN; --g_pulseDown; }
			if (g_pulseLS > 0) { out |= XINPUT_GAMEPAD_LEFT_THUMB; --g_pulseLS; }

			a_pad.wButtons = out;
		}

		DWORD WINAPI Chained(DWORD a_user, XINPUT_STATE* a_state)
		{
			if (GetCurrentThreadId() != g_gameThread) {
				if (g_otherThreadReads.fetch_add(1, std::memory_order_relaxed) == 0) {
					logger::info("pad: thread {} also reads the controller through the game's import - passed through untouched", GetCurrentThreadId());
				}
				return g_previous ? g_previous(a_user, a_state) : ERROR_DEVICE_NOT_CONNECTED;
			}
			if (a_user == 0) {
				if (auto cb = g_frameCallback.load(std::memory_order_acquire)) {
					cb();
				}
			}
			DWORD rc = g_previous ? g_previous(a_user, a_state) : ERROR_DEVICE_NOT_CONNECTED;
			if (!a_state || a_user != 0) {
				return rc;
			}
			if (rc != ERROR_SUCCESS) {
				XINPUT_STATE blank{};
				XINPUT_GAMEPAD probe{};
				if (!Inject(probe)) {
					return rc;
				}
				*a_state = blank;   // no pad connected, but a test step is running: hand over the step alone
				a_state->Gamepad = probe;
				rc = ERROR_SUCCESS;
			} else {
				Inject(a_state->Gamepad);
			}
			g_reads.fetch_add(1, std::memory_order_relaxed);
			const WORD rawButtons = a_state->Gamepad.wButtons;
			const BYTE rawLT = a_state->Gamepad.bLeftTrigger, rawRT = a_state->Gamepad.bRightTrigger;
			const SHORT rawRX = a_state->Gamepad.sThumbRX, rawRY = a_state->Gamepad.sThumbRY;
			Rewrite(a_state->Gamepad);
			const auto& g = a_state->Gamepad;
			if (g.wButtons != rawButtons || g.bLeftTrigger != rawLT || g.bRightTrigger != rawRT || g.sThumbRX != rawRX || g.sThumbRY != rawRY) {
				g_rewritten.fetch_add(1, std::memory_order_relaxed);
			}
			// the game only processes a state whose packet number moved: move it whenever what we hand over changes
			if (g.wButtons != g_prevOutButtons || g.bLeftTrigger != g_prevOutLT || g.bRightTrigger != g_prevOutRT || g.sThumbRX != rawRX || g.sThumbRY != rawRY) {
				++g_packetOffset;
				g_prevOutButtons = g.wButtons;
				g_prevOutLT = g.bLeftTrigger;
				g_prevOutRT = g.bRightTrigger;
			}
			a_state->dwPacketNumber += g_packetOffset;
			return rc;
		}

		std::string ModuleOf(const void* a_p)
		{
			HMODULE m = nullptr;
			wchar_t w[MAX_PATH]{};
			if (a_p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_p), &m) && m &&
				GetModuleFileNameW(m, w, MAX_PATH)) {
				return std::filesystem::path(w).filename().string();
			}
			return "an unknown module";
		}
	}

	bool Install()
	{
		if (g_installed.load()) {
			return true;
		}
		static bool s_tried = false;
		if (s_tried) {
			return false;
		}
		s_tried = true;
		g_gameThread = GetCurrentThreadId();   // Install runs at OBSE's post-load, on the game's main thread
		auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress) {
			logger::error("pad: the game has no import table - no controller rules");
			return false;
		}
		for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
			const char* dll = reinterpret_cast<const char*>(base + desc->Name);
			if (_strnicmp(dll, "xinput", 6) != 0) {
				continue;
			}
			auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
			auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++slots) {
				const bool match = IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)
				                       ? IMAGE_ORDINAL64(names->u1.Ordinal) == 2
				                       : std::strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, "XInputGetState") == 0;
				if (!match) {
					continue;
				}
				auto* slot = reinterpret_cast<XInputGetState_t*>(&slots->u1.Function);
				DWORD old = 0;
				if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
					logger::error("pad: the import slot could not be made writable ({})", GetLastError());
					return false;
				}
				g_previous = *slot;
				*slot = &Chained;
				VirtualProtect(slot, sizeof(void*), old, &old);
				g_previousTarget = ModuleOf(reinterpret_cast<const void*>(g_previous));
				g_installed.store(true);
				logger::info("pad: the game's {} XInputGetState import now runs the wheel's controller rules (previously {} in {})",
					dll, reinterpret_cast<const void*>(g_previous), g_previousTarget);
				return true;
			}
		}
		logger::error("pad: the game imports no XInputGetState - no controller rules");
		return false;
	}

	void Queue(const std::vector<Step>& a_steps)
	{
		std::scoped_lock l(g_injectLock);
		for (const auto& s : a_steps) {
			g_steps.push_back(s);
		}
	}

	bool MagicPanelUp()
	{
		return g_magicPanel;
	}

	std::size_t Queued()
	{
		std::scoped_lock l(g_injectLock);
		return g_steps.size() + (g_stepRunning ? 1 : 0);
	}

	void SetFrameCallback(FrameCallback a_callback)
	{
		g_frameCallback.store(a_callback, std::memory_order_release);
	}

	Status GetStatus()
	{
		return { g_installed.load(), g_reads.load(), g_rewritten.load(), g_previousTarget };
	}
}
