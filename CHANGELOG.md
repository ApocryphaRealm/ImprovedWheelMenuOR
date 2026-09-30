# Improved Wheel Menu (PerfectedWheelerOR) - changelog

Written as changes happen, not reconstructed afterwards (rule 61). Started 2026-09-29; the history before this file is
in `git log`. A version number is issued by the version gate only once a build is seen working in game (rule 48).

## Unreleased - 2026-09-29 - untested

### Round 2 (the owner's report, 2026-09-30)
- **Fixed: the ammo wheel never appeared.** The owner: "I don't see the ammo wheel while pressing D-pad right". The log
  showed every press arriving with a bow held, followed by "the wheel cannot be built yet". The player-controller
  lookup matched the engine's PlayerController class exactly. The game's controller is a subclass, so the lookup never
  found it and the widget was never created. It now accepts subclasses.
- **Added: an "Ammo Wheel" row on the game's Controller Controls page** (the owner: "make sure that you build the system
  row and that the game recognizes the key presses properly").
  - How it works: the plugin creates a runtime input action (IA_IWM_AmmoWheel) and maps it in IMC_Game_Default. The
    row is placed after the game's quick keys row, its default is D-pad right, and it is modelled on Tween Menu's
    Controls row.
  - Rebinding: the row can be rebound there to any controller button. The rebind is kept in [AmmoWheel] uButton,
    because the game's save cannot restore a runtime action. If the game re-applies its map without our action, the
    action is put back.
  - The press itself is still read from the controller.
- **Fixed: the Magic wheel now appears in the magic menu when you switch tabs to it from the inventory with the wheel
  panel showing.** The owner: "I switched tabs to the magic and the magic wheel doesn't appear."
  - Cause: the magic menu's panel reports no visibility change, so the plugin only learned it was showing from its own
    D-pad-down toggle, and the tab switch carries the panel across without one.
  - Fix: the panel's visibility is now read from the wheel widgets themselves (UWidget::IsVisible) five times a second
    while the magic menu is open.
- **Changed: a spell goes on one slot of the Magic wheel only**, as items already do on the Equipment wheel (the owner:
  "if I assign magic to a slot, I shouldn't be able to assign it the same magic to two other slots. Because it's
  redundant"). Assigning a spell to a slot moves it there from any other slot.

### Added
- the ammo wheel (the owner, 2026-09-29): a half wheel of up to eight arrow kinds locked to the middle of the right-hand
  edge, opened with D-pad right while a bow is held, in gameplay only ("It would default to D-pad right while holding a
  bow"). The right stick or D-pad up / down points (it opens on the arrows worn now, drawn gold); D-pad right again or A
  equips the pointed arrows, B closes without. The equip runs on the TES thread (TesThread.cpp, from Simple Loadout
  System - the equipment path traps every other thread) with no lock. `[AmmoWheel] bEnabled / uButton / uScalePercent`.
  Built from the owner's description; Wheeler Refined's ammo wheel source is GPL-3.0-only and was not read (rule 62).
  Its Controls-page row comes with the next round - the button is an INI setting until then.
- Y on arrows in the inventory puts them on the ammo wheel, never the Equipment wheel, and arrows already on the
  Equipment wheel (or put on a key by the game's own assign) move over, taking the game's key with them, and the
  radial's pictures are patched so nothing stale is left on it. Each wheel holds only its own kind: items, spells, arrows.
  Favourited arrows cannot be dropped or sold, as other favourites.
- the centre rest snap (the owner: "we'd have to add the center rest snap feature or you might select items
  mistakenly"): on the HUD radial and the ammo wheel, once the right stick has pointed, bringing it back to rest in the
  middle for `uRestSnapMs` (150 ms) points at no slot, so letting go then uses nothing. `[Wheel] bCentreRestSnap`.

### Changed
- taking an item off the wheel the normal way unfavourites it (the owner, 2026-09-29: "I didn't press Y to unfavorite
  the item. I just removed the item from the wheel the normal way. Which should be considered going forward."). A removal
  through the assign panel (assign again on the item, or on an entry the slot holds but is not showing) now takes it off
  every slot of that wheel, and so does Y off (it took the item off the first slot holding it only). The case: the
  Revealer of Iniquity, taken off slots 7 and 1, stayed an undroppable favourite as the first of Equipment slot 3's three
  entries, where nothing showed it.

### Fixed
- the crash of 2026-09-29 09:15:50 when an item was dropped with X (TestBench crash record: ImprovedWheelMenu.dll+0x1B668,
  `reflect::IsLive` called from `rows::HighlightedItem`, reading freed memory at the row's internalIndex). The wheel
  kept inventory and magic rows, their icons and the quick keys view model from earlier frames and checked them by
  reading the object's own index - garbage once the row is freed. Every object kept across frames is now held with the
  object-array slot it was found in (`reflect::Handle`) and checked by asking the slot, never by reading the object; a
  highlighted row that is gone reads as "no item".
- (1445bc7, earlier the same day) `reflect::Text` / `TextKey` refuse a zeroed FText - a list row never given an item.
