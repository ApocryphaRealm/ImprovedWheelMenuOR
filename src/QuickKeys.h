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
	// any wheel widget's own visibility says shown (UWidget::IsVisible). The magic menu's panel reports no visibility
	// change through ProcessEvent, so in the magic menu this is how its panel is known to be up. Game thread.
	bool AnyWheelVisible();
	int  PointedSlot();  // the slot under the pointer on whichever is showing, -1 for none
	void CancelChoice(); // B on the HUD radial: the view model points at no slot, so the close uses nothing

	// The eight slot pictures the radial and the menu panels draw (VQuickKeysMenuViewModel::Icons, by key 0-7).
	// WriteIcons pushes a set through the view model's own SetIcons on every instance; returns how many took it.
	using Icons = std::array<UE::UObject*, 8>;
	Icons ReadIcons();
	int   WriteIcons(const Icons& a_icons);
	// draws straight onto every wheel widget - only the slots where a_icons differs from a_shown (what the widgets
	// show now); returns the pictures set
	int   DrawIcons(const Icons& a_icons, const Icons& a_shown);
	bool  TakeGameDrew();   // the game drew its own pictures since the last call (the widgets show the view model's again)

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
