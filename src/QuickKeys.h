#pragma once

// ============================================================================================================
// The game's Quick Keys radial, observed. Facts from the runtime research of 2026-09-26 (plans\perfected-wheeler-
// oblivion\RESEARCH.md, "Settled at runtime"):
//
//   * The HUD radial is ONE widget for the session, class WBP_ModernMenu_QuickKeys_C, shown and hidden. The
//     inventory and the magic menu build their OWN instance of the same class - the assign panel on the right of
//     the menu (shown with the left stick click, "Show Shortcuts").
//   * What passes through UObject::ProcessEvent: OnVisibilityChangedEvent(4) as an instance shows, "Update Key
//     Index"(N) each time the pointer crosses into a slot (-1 = none), OnVisibilityChangedEvent(1) as it hides - on
//     the HUD radial the game uses the chosen slot on that close. The chosen slot is the view model's KeyIndex.
//   * KeyIndex numbering: clockwise, 45 degrees a slot, 0 top-left, 1 top ... 7 left.
//
// Observed through pe::Watch (a vtable entry swap - coexists with UE4SS). An instance that shows while the
// inventory or the magic menu is active is that menu's PANEL; otherwise it is the HUD RADIAL.
// ============================================================================================================

namespace quickkeys
{
	enum class Event
	{
		kOpened,        // the HUD radial appeared
		kPointed,       // a slot is under the pointer (radial or panel; slot = -1 when it left every slot)
		kClosed,        // the HUD radial went away; slot = the slot the game uses
		kPanelOpened,   // a menu's assign panel appeared
		kPanelClosed,   // a menu's assign panel went away
	};

	using Listener = void (*)(Event a_event, int a_slot);

	// Call once, after OBSE's post-load. The watch is installed lazily, once the widget's class exists.
	void Install(Listener a_listener);
	void Tick();

	bool RadialOpen();   // the HUD radial is showing
	bool PanelOpen();    // a menu's assign panel is showing
	int  PointedSlot();  // the slot under the pointer on whichever is showing, -1 for none

	struct Status
	{
		bool hookInstalled = false;
		bool widgetClassFound = false;
		bool viewModelFound = false;
		bool open = false;        // HUD radial
		bool panelOpen = false;   // a menu's panel
		int  pointedSlot = -1;
		int  lastChosenSlot = -1;
		std::uint32_t opens = 0;
		std::string   problem;
	};
	Status GetStatus();
}
