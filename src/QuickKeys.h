#pragma once

// ============================================================================================================
// The game's Quick Keys radial, observed. Facts from the runtime research of 2026-09-26 (plans\perfected-wheeler-
// oblivion\RESEARCH.md, "Settled at runtime"):
//
//   * The radial is ONE UMG widget for the session, class WBP_ModernMenu_QuickKeys_C, shown and hidden - never
//     created per open. Its view model is the one live VQuickKeysMenuViewModel.
//   * What passes through UObject::ProcessEvent when it is used: OnVisibilityChangedEvent(4) as it opens,
//     "Update Key Index"(N) each time the pointer crosses into a slot (-1 = none), OnVisibilityChangedEvent(1) as it
//     closes - and the chosen slot is used by the game on that close. SetKeyIndex / RegisterSendSelectedQuickKey are
//     native-to-native and never reach ProcessEvent, so the chosen slot is read from the view model's KeyIndex.
//   * KeyIndex numbering: clockwise, 45 degrees a slot, 1 at the TOP (0 top-left ... 3 right ... 5 bottom ... 7 left).
//
// This module hooks ProcessEvent (vtable slot 0x4D of the widget - the shared UObject::ProcessEvent), filters on the
// widget's class, and turns those calls into three events for the rest of the mod. Everything is NULL-guarded and
// records its result for the self-check (memory: guards-and-a-selfcheck-report-in-every-build).
// ============================================================================================================

namespace quickkeys
{
	enum class Event
	{
		kOpened,   // the radial appeared
		kPointed,  // a slot is under the pointer (slot = -1 when it left every slot)
		kClosed,   // the radial went away; slot = the view model's KeyIndex at that moment (the slot the game uses)
	};

	using Listener = void (*)(Event a_event, int a_slot);

	// Call once, after OBSE's post-load. The hook itself is installed lazily, on the first Tick that finds the
	// widget's class loaded (the class exists once the HUD is built, i.e. after a save is loaded).
	void Install(Listener a_listener);

	// Once per frame from any thread that runs every frame (the framework's HUD callback). Cheap when nothing is
	// left to do.
	void Tick();

	// State for the settings page and the self-check.
	struct Status
	{
		bool hookInstalled = false;
		bool widgetClassFound = false;
		bool viewModelFound = false;
		bool open = false;
		int  pointedSlot = -1;
		int  lastChosenSlot = -1;
		std::uint32_t opens = 0;
		std::string   problem;   // the last refusal, in words, or empty
	};
	Status GetStatus();
}
