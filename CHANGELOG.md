# Improved Wheel Menu (PerfectedWheelerOR) - changelog

Written as changes happen, not reconstructed afterwards (rule 61). Started 2026-09-29; the history before this file is
in `git log`. A version number is issued by the version gate only once a build is seen working in game (rule 48).

## Unreleased - 2026-09-29 - untested

### Round 3 (in progress, 2026-09-30)
- **Changed: A chooses and closes on the Magic wheel, as on the game's own** (the owner: "I tried selecting clairvoyance
  with pressing A like the vanilla game does and it didn't close the wheel ... it only selected it when I pressed the right
  bumper it should do both"). A on the radial's Magic wheel sets the pointed slot's spell and lets the wheel close, like
  RB; the game never sees that A. On the inventory wheel A stays the game's own.
- **Fixed: the inventory wheel showed none of its equipment** (the owner: "the inventory wheel is not showing the
  equipment on the wheel"). The primary's read at 04:37: the game's picture list (the view model's Icons) was EMPTY while
  the keys held items, so the game set every slot's opacity to 0 on both wheels' material instances; the old redraw
  (SetQuickKeyByIndex, a click per slot) only swapped pictures in. The inventory wheel is now drawn from the keys on the
  wheel's own instance - each slot's IDn texture and opacity, silently, checked every 100 ms while the wheel is up;
  empty keys and spells stay empty. The widget redraw is off.
- **Fixed: closing the radial on a Magic slot also used the game's key there** (04:37:34 and 04:37:38: "the game uses
  slot 3" right after a Magic choice). The radial takes a moment to close and the stick, still pointing, pointed the slot
  again; the stick is now held centred and the choice kept cleared until the radial has closed.
  - Known: choosing a Magic slot does not change the HUD's spell - SetCurrentSpell sets the spell being cast, not the
    selected one (selectedSpell stayed the old spell). The game's own spell-select path is being probed.
- **Added: the favourites column in the magic menu** (the owner: "we need to add a star for favorites on the column in
  the magic menu"). Every magic row gets the same star at the end of its Magic_entry_horizontal: filled white while the
  spell is on the Magic wheel, and a click puts it on or takes it off the Magic wheel as Y does. The inventory column
  and this one share one module (Favourites.cpp); the owner confirmed the inventory's works and is interactive.
- **Fixed: items favourited back onto the inventory wheel stayed invisible** (the owner: after he "deselected the weapons
  from the wheel and tried favoriting them back, they didn't appear in the wheel menu"). The game keeps a slot's old
  picture after an item leaves its key, and a key with a picture and no item was read as a spell and its picture's
  opacity set to 0 on the game's wheel - never put back, so whatever went there next stayed invisible (04:18:54, the
  mace's old slot 7). A key now counts as a spell only when its picture is one of the game's magic icons (the texture's
  own path, /icons/magic/), and a blanked slot gets the game's opacity back as soon as it stops being one.
- The wheels stand down while Tween Menu's menu is open (TweenMenu_IsOpen, Tween Menu 1.0.1).
- **Fixed: the Magic wheel never drew, and the favourites column never appeared** (the owner's screenshots of 03:38:40
  and 03:38:46: the magic menu's wheel still showed a bow, a sword, a torch; the log: "the game's wheel picture is NOT
  FOUND (quickKeys_material)", "a row has no inv_entry_horizontal - no column"). GetWidgetFromName found neither widget
  in game. Both are now also found by walking the widget trees ourselves - each user widget's WidgetTree root, every
  panel's Slots[].Content, nested user widgets' trees - and comparing names in the plugin (ui::FindInTree).
- **Fixed: "Magic Wheel" stayed over the magic menu after its wheel panel was hidden** (the owner: "after you hide
  the wheel menu, the title of it still stays"). The panel is hidden through a parent, so the wheel's own IsVisible
  stayed true and the panel read as up again 0.1 s after B closed it. A wheel now counts as on screen only when it and
  every widget above it is visible and not faded out (ui::ShownOnScreen).
- **Added: the favourites column in the inventory** (the owner: "I still don't see the favorites column in the inventory
  interface ... Maybe we can build it just like we did the buttons for the SLS mod", and "each item have a button. In
  its column with an empty star shape then the star shape gets filled to be white whenever it's favorited").
  Favourites.cpp adds one more column at the end of every inventory row's inv_entry_horizontal: the game's own empty
  star (T_UI_star_default_D) over a white fill of ours (ImprovedWheelMenu\FavouriteStarFill.png - an original shape
  drawn for the mod, imported at run time with ImportFileAsTexture2D; Slate clamps a tint, so the game's blue star
  could not be tinted white), and the game's invisible button over both, as Simple Loadout System's boxes use it -
  a click toggles the favourite exactly as Y does (never a stop for the controller: IsFocusable off). Rows are reused
  as the list scrolls: each keeps its column, and only a row whose item or favourite state changed is redrawn, looked
  at every 100 ms while the inventory is open and only when a row or a wheel moved.
- **Changed: the Magic wheel is a wheel of its own** (the owner: "creating a second wheel, not just a renamed wheel, a
  second entirely different wheel, similar to how we created the ammo wheel. And this one needs to be specific to the
  magic inventory menu. Because when I go to inventory and magic, I still see all the same items. And only on the
  magic wheel do I hear a bunch of ticking noises as soon as it appears"). MagicWheel.cpp: its own image with its own
  instance of the game's wheel material (of the same parent as the game's), laid exactly over the game's wheel picture
  (quickKeys_material > Image, measured every 100 ms) while the Magic wheel is up - the magic menu's panel, or the HUD
  radial with Magic active - with the game's picture hidden under it (its opacity put back after). Its eight slots are
  the Magic wheel's spells, empty ones drawn empty; every scalar of the game's instance but the slots' own is copied
  onto ours, so the selector and its animation follow the game's exactly. Nothing is written into the game's view model
  any more - the old SetIcons redrew slots, and every redrawn slot ticked.
- **Changed: no magic on the inventory wheel** (the owner: "the inventory wheel menu still having magic on it"). A game
  key with a picture and no item - a spell the game kept from before - is drawn empty on the game's wheel (its slot's
  opacity on the game's own instance, set again whenever the game redraws), and closing the radial on it uses nothing.
- Spell pictures also come from the spell's first effect's own icon (TESIcon, the same path rule as items), so the
  Magic wheel has pictures before the magic menu has shown its rows.
- **Fixed: the ammo wheel showed the material's defaults** (the owner: no arrows, and "box number eight is highlighted
  permanently, and it doesn't respond to my right stick movement"). The primary's read with the wheel open found no
  instance of ours - only the HUD's two - so every parameter written went nowhere. Each open now asks the wheel's image
  what its brush draws; when it is not our instance, the instance is made again and set (a warning names what it
  found). With nothing pointed - an empty wheel - the selector goes to slot 3 in the hidden right half, instead of the
  material's default slot 8.
- **Fixed: no arrow pictures until the inventory had shown them.** An item's picture now also comes from its own form:
  its icon path (TESIcon, "Weapons\IronArrow.dds") names the remaster's texture - /Game/Art/UI/Icons/Dynamic_Icons/
  menus/icons/weapons/T_ironarrow, the folder in lower case, "T_" and the lower-case stem (the primary session's reads
  and the paks) - loaded when first asked. Arrows, weapons, books, misc items, apparatus, ingredients, potions and
  lights; a "rows: icon for ..." line names each one loaded, and a failure is logged once and not retried.
  - The 02:37 empty wheel was the owner's Y presses in the inventory removing both arrows (Y toggles a favourite).
- **Changed: the ammo wheel is the game's own wheel, sliced in half** (the owner: "the arrows icons and slot outline is
  too small for my liking and the highlighting effect is too dim ... basically just a carbon copy of the game's wheel
  menu just sliced in half").
  - The HUD wheel is one image drawn by a dynamic instance of MIC_UI_QuickKeys (the primary session's reads). The ammo
    wheel is a second instance of it, 634 across like the HUD's, clipped just past its middle, so the left five slots
    stay whole: 1 at the top, 8, 7 at the left, 6, 5 at the bottom.
  - The arrows go into those slots' ID textures, and the game's own selector (SelectorRotator, SelectorArrowAlpha)
    points at the chosen one.
  - Entries fill from the middle: one at 7, two at 8 and 6, up to five arrow types.
  - The drawn circles remain as the fallback if the material can't be instanced.
- **Added: the wheel's name over the wheel on screen**: "Inventory Wheel" or "Magic Wheel" over the HUD radial (the
  active wheel) and over the inventory's and the magic menu's assign panels (the owner: "The wheel menus aren't named in
  the inventory when they should be").
  - The last session's log shows the magic menu's wheel WAS drawn as the Magic wheel ("0 of 8 slots hold a spell ... the
    rest drawn empty"). An unnamed empty wheel read as "no Magic wheel" ("I still haven't seen the wheeler wheel for the
    magic menu").
  - The label is the game's own text prefab, placed from the wheel widget's cached geometry (Minimap Menu's compass
    placement). It is checked ten times a second, and its world-context call is fault-guarded.
- Still open, waiting on in-game reads: the favourites column (the list rows' widget layout), and keeping spells off
  the inventory wheel (where the game stores a spell's own quick key has not been found yet).
- **Changed: the ammo wheel divides the half circle evenly, every entry the same size** (the owner, testing 465ca21: "The
  iron and steel arrow overlap in the ammo wheel, and there are different sizes too, which is weird. It should be dividing
  up the circumference evenly not pushing them both into the center").
  - n entries each take one of n equal sectors from straight up to straight down, sitting at its middle: one in the
    middle, two at 135 and 225 degrees, eight 22.5 degrees apart. This replaces the centred 22.5-degree spacing.
  - The pointed entry is lit, not enlarged.
- **Changed: no centre rest snap on the ammo wheel** (the owner: "otherwise it's inconvenient to use it"). The pointed
  arrows stay pointed when the stick is let go. The main radial keeps its snap.
- **Changed: the ammo wheel lays out as the Skyrim Perfected Wheeler's does** (the owner: "if you only have one arrow
  selected in the ammo wheel or one arrow favorited to it, then it centers to the middle of the semicircle and for every
  additional favorited arrow type it adds an additional radial entry and each arrow should have the same circle art as
  the main wheel for each arrow slot").
  - One entry per favourited arrow type, centred on the middle of the half circle and 22.5 degrees apart, closer only
    when the entries would reach the screen's edge.
  - Every entry sits on the main wheel's own slot circle, which grows and warms when pointed. The primary session's
    ue.tree showed the HUD wheel is ONE image drawn by the material MIC_UI_QuickKeys. The circle it draws each slot
    with, T_QuickKeys_SingleCircle_D (a gold rim, a dark translucent inside), was found in the paks with uetex and is
    loaded by path (KismetSystemLibrary's LoadAsset_Blocking, Minimap Menu's loader). If it can't be loaded, a drawn
    disc stands in. The circles show 0.2 R across, so eight entries never overlap.
  - The right stick picks the nearest entry, and D-pad up / down step through the entries.
- **Fixed: the magic menu showed the inventory wheel's pictures** (the owner: "I still don't see the magic wheel in the
  magic inventory" - "the magic menu wheel is a distinct separate wheel from the inventory wheel. And just like the
  inventory wheel, even if it's empty, it's still visible and it still draws. It would just be empty").
  - Cause: the owner's Magic wheel is empty (0 entries), and the widget's own SetQuickKeyByIndex keeps the old picture
    when given none, so every empty Magic slot went on showing the inventory wheel's item.
  - Fix: the Magic wheel is now written into the wheel's view model (SetIcons), where the game's own drawing shows an
    empty key as empty. It is written again whenever the game pushes its own pictures, and only then, because each
    redraw clicks. The game's eight pictures are kept and written back when the Magic wheel leaves the screen.
  - The log says how many Magic slots hold a spell and how many of those have their picture known.
- **Fixed: iron arrows duplicated in the inventory** (the owner: "There seems to be a weird duplication bug with the
  iron arrows in the inventory"). Every close of the ammo wheel with A or its button equipped the pointed stack again,
  even when it was already worn: 40 whole-stack equips of the same iron arrows in one session. Arrows already worn are
  now left alone, and every real equip logs the stack's data lists afterwards (how many, how many worn, their counts), so
  a split stack shows in the log. A stack already split in this save may need a reload.
- **Fixed: the arrows sat partly off screen on the ammo wheel** (the owner: "the arrows appear slightly off screen in
  the arrow ammo wheel"). The eight arrows were spread over the whole half circle, so the top and bottom ones touched
  the screen's edge. They now sit on the arc from 112.5 to 247.5 degrees, at 62% of the radius, and the right stick
  picks the nearest arrow on that arc.
- **Changed: the wheels stand down while the Apocrypha Menu Framework's window is open** (the owner: treat it like the
  game's own wheel, apart from the bow condition). IWM reads the pad on the game's import before the framework's gate
  hides it from the game, so D-pad down and D-pad right still opened the wheels with the framework's window up. IWM now
  looks up the framework's AMF_IsMenuOpen export (AMF OR 1.0.5+). While the window is open, no wheel rule runs, an open
  ammo wheel closes and a latched wheel lets go. A framework without the export leaves the rules as they were.

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
