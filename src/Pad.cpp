#include "Pad.h"

#include "Menus.h"
#include "QuickKeys.h"
#include "Wheels.h"

#include <Xinput.h>   // types and constants only - nothing is linked or loaded

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
			const menus::Menu menu = menus::Active();
			if (menu != g_prevMenu) {
				ResetMenuState();
				if (menu != menus::Menu::kNone) {
					g_latched = false;       // a menu opening ends a latched wheel
					g_suppressDown = false;
				}
				g_prevMenu = menu;
			}
			WORD out = raw;

			if (menu != menus::Menu::kNone) {
				// ---- the inventory / magic menu ----
				out &= ~XINPUT_GAMEPAD_Y;
				if (pressed & XINPUT_GAMEPAD_Y) {
					wheels::Favourite(menu);
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
				if (quickkeys::PanelOpen()) {
					// B backs out of the panel (the game's own toggle, as the hold on D-pad down); the next B leaves the menu
					if (pressed & XINPUT_GAMEPAD_B) {
						g_swallowB = true;
						g_pulseLS = kPulseReads;
						logger::info("pad: B with the assign panel showing - panel closed");
					}
					// the game's A (Assign Item) is watched: pressed again on the same item it removes it (Wheels.h)
					if (pressed & XINPUT_GAMEPAD_A) {
						wheels::AssignPressed(menu, quickkeys::PointedSlot());
					}
					out &= ~(XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
					const auto wheel = menu == menus::Menu::kMagic ? wheels::Wheel::kMagic : wheels::Wheel::kEquipment;
					if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) { wheels::CycleEntry(wheel, quickkeys::PointedSlot(), -1); }
					if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) { wheels::CycleEntry(wheel, quickkeys::PointedSlot(), +1); }
				}
			} else {
				// ---- gameplay ----
				if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) {
					if (g_latched) {
						g_latched = false;
						g_suppressDown = true;   // the game sees the button go up: the wheel closes on the pointed slot
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
					}
				}
				out &= ~XINPUT_GAMEPAD_DPAD_DOWN;
				if (((raw & XINPUT_GAMEPAD_DPAD_DOWN) && !g_suppressDown) || g_latched) {
					out |= XINPUT_GAMEPAD_DPAD_DOWN;
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
					const int slot = quickkeys::PointedSlot();
					out &= ~(XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
					if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) { wheels::SwitchWheel(-1); }
					if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) { wheels::SwitchWheel(+1); }
					if (ltPressed) { wheels::CycleEntry(wheels::Active(), slot, -1); }
					if (rtPressed) { wheels::CycleEntry(wheels::Active(), slot, +1); }
					if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) { wheels::RemoveEntry(wheels::Active(), slot); }
					if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) {
						wheels::UseNow(slot);
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
			const DWORD rc = g_previous ? g_previous(a_user, a_state) : ERROR_DEVICE_NOT_CONNECTED;
			if (rc != ERROR_SUCCESS || !a_state || a_user != 0) {
				return rc;
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

	Status GetStatus()
	{
		return { g_installed.load(), g_reads.load(), g_rewritten.load(), g_previousTarget };
	}
}
