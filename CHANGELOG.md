# Improved Wheel Menu (PerfectedWheelerOR) - changelog

Written as changes happen, not reconstructed afterwards (rule 61). Started 2026-09-29; the history before this file is
in `git log`. A version number is issued by the version gate only once a build is seen working in game (rule 48).

## Unreleased - 2026-09-29 - untested

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
