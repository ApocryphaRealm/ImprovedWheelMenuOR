Improved Wheel Menu
===================
Version 1.0.0

Oblivion Remastered's own quick-key radial, made into a full wheel menu: eight slots that each hold several items, a
Magic wheel of its own for spells, and an ammo wheel for your arrows while a bow is held. The game's own radial, button
prompts and quick keys are used throughout; nothing of the vanilla interface is replaced.

WHAT YOU GET
------------
  * The inventory wheel: hold the game's quick-key button to open the radial and point with the right stick. Each of
    the eight slots holds up to five items (uEntriesPerSlot); the counter under the wheel shows which place the shown
    item has and how many the slot holds. LT and RT choose the item within the pointed slot, RB or A use it, and
    D-pad left and right switch between the inventory and Magic wheels. Pointing alone never uses anything, letting
    go of the wheel button uses nothing, and B closes the wheel.
  * The centre rest snap (on by default): with the stick back at rest in the middle, no slot is pointed, so letting go
    of the stick never picks one by mistake.
  * Putting things on the wheel: Y on an item in the inventory, or on a spell in the magic menu, makes it a favourite
    and puts it on its wheel; Y again takes it off. A favourite cannot be sold or dropped. To place it yourself, hold
    D-pad down for the game's assign panel, point a slot, choose its place with D-pad left and right, and press A.
    The game's own sorting moves from Y to the left stick click.
  * The Magic wheel: a separate wheel for spells, always drawn as itself. A pick makes the spell the one the cast
    button casts, and the HUD's quick-magic picture follows it.
  * The ammo wheel: with a bow held, D-pad right opens a half wheel on the right-hand edge with the arrows you have
    put on it; the right stick or D-pad up and down point, the button again or A equips, B closes. Its button is
    rebindable from its own row on the game's Controls page.
  * Settings in ImprovedWheelMenu.ini: entries per slot, the rest snap and its time, the ammo wheel on/off, button
    and size, the log level.

KNOWN ISSUES
------------
  * There is no settings page; the INI holds the few settings there are.
  * Controller first: the wheel is the game's controller radial. On keyboard and mouse the game's own quick keys work
    as before.

INSTALLATION
------------
  * Everything goes under OblivionRemastered\Binaries\Win64\OBSE\Plugins\: ImprovedWheelMenu.dll, .pdb, .ini and the
    ImprovedWheelMenu folder (the favourite star's fill picture).
  * Install with your mod manager or drop the OblivionRemastered folder over the game's own. Mod Organizer 2 users
    need the folder installed at the game's root (Root Builder or an equivalent).

DEBUGGING
---------
  Send the log with any bug report: Documents\My Games\Oblivion Remastered\OBSE\Logs\ImprovedWheelMenu.log. Set
  [Log] uLogLevel=1 in ImprovedWheelMenu.ini for more detail first.

REQUIREMENTS
------------
  * OBSE64 (Nexus 282), 0.2.2 or newer
  * Address Library for OBSE Plugins (Nexus 4475)

LICENCE
-------
  GPL-3.0-or-later (LICENSE, NOTICE.md). Built on CommonLibOB64 (GPL-3.0); spdlog and the other linked components are
  listed with their notices in THIRD_PARTY_NOTICES.md. Source: https://github.com/ApocryphaRealm/ImprovedWheelMenuOR
