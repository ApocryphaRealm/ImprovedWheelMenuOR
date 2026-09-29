#include "Wheels.h"

#include "Inventory.h"
#include "QuickKeys.h"
#include "Rows.h"
#include "Settings.h"

#include <fstream>
#include <sstream>

namespace wheels
{
	namespace
	{
		constexpr int kEquip = 0;
		constexpr int kMagic = 1;

		std::atomic<int> g_active{ static_cast<int>(Wheel::kEquipment) };

		struct Slot
		{
			std::vector<std::uint32_t> entries;   // formIDs, in the order they were added
			int active = -1;                      // index into entries, -1 = none
		};
		using WheelSlots = std::array<Slot, inventory::kSlots>;
		std::array<WheelSlots, 2> g_wheels;
		std::string g_loadedFor;   // the character g_wheels belongs to

		// what the wheel's pictures are showing now
		enum class Shown { kGame, kMagic };
		Shown g_shown = Shown::kGame;

		std::string NameOf(int a_wheel, std::uint32_t a_id)
		{
			return a_wheel == kMagic ? rows::SpellName(a_id) : inventory::NameOf(a_id);
		}

		// ---- persistence ----

		std::filesystem::path FileFor(const std::string& a_name)
		{
			std::string safe;
			for (const char c : a_name) {
				safe += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') ? c : '_';
			}
			return settings::PluginFolder() / L"ImprovedWheelMenu" / (safe + ".txt");
		}

		void Save()
		{
			if (g_loadedFor.empty()) {
				return;
			}
			const auto path = FileFor(g_loadedFor);
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			std::ofstream f(path, std::ios::trunc);
			if (!f) {
				logger::error("wheels: could not write {}", path.string());
				return;
			}
			f << "# Improved Wheel Menu - the wheels of " << g_loadedFor << "\n";
			f << "# <wheel> <slot 1-8> active=<entry, 0 = none> <formID>...\n";
			for (int w = 0; w < 2; ++w) {
				for (int s = 0; s < inventory::kSlots; ++s) {
					const Slot& slot = g_wheels[w][s];
					if (slot.entries.empty()) {
						continue;
					}
					f << (w == kEquip ? "Equipment" : "Magic") << ' ' << (s + 1) << " active=" << (slot.active + 1);
					for (const auto id : slot.entries) {
						f << std::format(" 0x{:08X}", id);
					}
					f << '\n';
				}
			}
		}

		void Load(const std::string& a_name)
		{
			g_wheels = {};
			g_loadedFor = a_name;
			const auto path = FileFor(a_name);
			std::ifstream f(path);
			if (!f) {
				logger::info("wheels: no saved wheels for {} yet ({})", a_name, path.filename().string());
				return;
			}
			std::string line;
			int rows = 0;
			while (std::getline(f, line)) {
				if (line.empty() || line[0] == '#') {
					continue;
				}
				std::istringstream in(line);
				std::string wheel, activeText;
				int slotNo = 0;
				if (!(in >> wheel >> slotNo >> activeText) || slotNo < 1 || slotNo > inventory::kSlots || !activeText.starts_with("active=")) {
					logger::warn("wheels: unreadable line in {}: {}", path.filename().string(), line);
					continue;
				}
				Slot& slot = g_wheels[wheel == "Magic" ? kMagic : kEquip][slotNo - 1];
				std::string id;
				while (in >> id) {
					slot.entries.push_back(static_cast<std::uint32_t>(std::stoul(id, nullptr, 16)));
				}
				slot.active = std::clamp(std::atoi(activeText.c_str() + 7) - 1, -1, static_cast<int>(slot.entries.size()) - 1);
				++rows;
			}
			logger::info("wheels: loaded {} slot(s) for {}", rows, a_name);
		}

		int IndexOf(const Slot& a_slot, std::uint32_t a_id)
		{
			const auto it = std::find(a_slot.entries.begin(), a_slot.entries.end(), a_id);
			return it == a_slot.entries.end() ? -1 : static_cast<int>(it - a_slot.entries.begin());
		}

		int Cap() { return std::max(1, settings::Get().entriesPerSlot); }

		// The game's keys are the truth for each Equipment slot's active entry (the save carries them).
		void Reconcile()
		{
			const auto keys = inventory::Keys();
			for (int s = 0; s < inventory::kSlots; ++s) {
				Slot& slot = g_wheels[kEquip][s];
				if (!keys[s]) {
					slot.active = -1;
					continue;
				}
				int idx = IndexOf(slot, keys[s]);
				if (idx < 0) {
					if (static_cast<int>(slot.entries.size()) >= Cap()) {
						slot.entries.pop_back();
					}
					slot.entries.push_back(keys[s]);
					idx = static_cast<int>(slot.entries.size()) - 1;
					logger::info("wheels: Equipment slot {} picked up {} from the game", s + 1, inventory::NameOf(keys[s]));
				}
				slot.active = idx;
			}
		}

		bool EnsureLoaded()
		{
			if (!RE::PlayerCharacter::GetSingleton()) {
				return false;
			}
			const std::string name = inventory::PlayerName();
			if (name != g_loadedFor) {
				Load(name);
			}
			Reconcile();
			return true;
		}

		// ---- pictures ----

		// One view model feeds the HUD radial and both menu panels (a single live instance, read 2026-09-29), and the
		// game pushes its own eight pictures into it just AFTER one of them opens. So:
		//   Equipment - the game's pictures already are the Equipment wheel (its keys); only a slot we change is patched.
		//   Magic     - while it shows, every controller read puts the Magic wheel's pictures back if the game replaced
		//               them, keeping what the game pushed so it goes back when the Magic wheel leaves the screen.

		quickkeys::Icons MagicIcons()
		{
			quickkeys::Icons icons{};
			for (int k = 0; k < inventory::kSlots; ++k) {
				const Slot& slot = g_wheels[kMagic][k];
				if (slot.active >= 0) {
					icons[k] = rows::SpellIcon(slot.entries[slot.active]);
				}
			}
			return icons;
		}

		// The game keeps its own copy of the eight pictures and refreshes it only when IT assigns a key, so a slot whose
		// key we moved goes black (read 2026-09-29). While the wheel is on screen, each Equipment slot holding an item
		// whose picture a row has shown is kept at that picture (throttled: the inventory walk is not free).
		// What is DRAWN is set on the wheel widgets themselves (quickkeys::DrawIcons); the view model keeps the game's
		// own eight pictures untouched, so it is always the truth for "the game's keys". Redrawn when the wanted set
		// changes, and every 250 ms while shown (the game redraws the widget from the view model when it refreshes).
		quickkeys::Icons                      g_lastDrawn{};
		std::chrono::steady_clock::time_point g_nextRedraw{};

		void Draw(const quickkeys::Icons& a_icons, bool a_force)
		{
			const auto now = std::chrono::steady_clock::now();
			if (!a_force && a_icons == g_lastDrawn && now < g_nextRedraw) {
				return;
			}
			g_nextRedraw = now + 250ms;
			g_lastDrawn = a_icons;
			quickkeys::DrawIcons(a_icons);
		}

		// The game refreshes its pictures only when IT assigns a key, so a slot whose key we moved shows the old or no
		// picture (read 2026-09-29). While the wheel is on screen, each Equipment slot holding an item whose picture a
		// row has shown is drawn with that picture; every other slot keeps the game's.
		std::chrono::steady_clock::time_point g_nextEquipCheck{};

		void AssertEquipment()
		{
			if (!(quickkeys::RadialOpen() || quickkeys::PanelOpen())) {
				return;
			}
			const auto now = std::chrono::steady_clock::now();
			if (now < g_nextEquipCheck) {
				return;
			}
			g_nextEquipCheck = now + 100ms;   // the inventory walk is not free
			const auto keys = inventory::Keys();
			auto icons = quickkeys::ReadIcons();
			bool ours = false;
			for (int k = 0; k < inventory::kSlots; ++k) {
				auto* icon = keys[k] ? rows::ItemIcon(keys[k]) : nullptr;
				if (icon && icons[k] != icon) {
					icons[k] = icon;
					ours = true;
				}
			}
			if (ours || icons != g_lastDrawn) {
				Draw(icons, false);
			}
		}

		void AssertMagic()
		{
			if (g_shown != Shown::kMagic) {
				AssertEquipment();
				return;
			}
			Draw(MagicIcons(), false);
		}

		void ShowMagic(bool a_on)
		{
			if ((g_shown == Shown::kMagic) == a_on) {
				return;
			}
			if (a_on) {
				g_shown = Shown::kMagic;
				const auto icons = MagicIcons();
				Draw(icons, true);
				int drawn = 0;
				for (auto* i : icons) {
					drawn += i ? 1 : 0;
				}
				logger::info("wheels: the wheel shows the Magic wheel ({} of 8 slots with a picture)", drawn);
			} else {
				g_shown = Shown::kGame;
				Draw(quickkeys::ReadIcons(), true);   // the game's own, from its untouched view model
				logger::info("wheels: the wheel shows the game's own keys again");
			}
		}

		// an Equipment slot's key changed under the game: its picture follows (when a row has shown that item's icon)
		void PatchEquipment(int a_slot)
		{
			if (g_shown == Shown::kMagic || a_slot < 0 || a_slot >= inventory::kSlots) {
				return;
			}
			const auto id = inventory::Keys()[a_slot];
			auto* icon = id ? rows::ItemIcon(id) : nullptr;
			if (id && !icon) {
				logger::info("wheels: slot {}'s picture not updated - no row has shown {} this session", a_slot + 1, inventory::NameOf(id));
				return;
			}
			g_nextEquipCheck = {};   // at once, not on the next 100 ms tick
			AssertEquipment();
		}

		void Refresh()
		{
			AssertMagic();
		}

		// every Equipment slot whose key moved since a_before gets its picture
		void PatchChanged(const inventory::KeyMap& a_before)
		{
			const auto now = inventory::Keys();
			for (int k = 0; k < inventory::kSlots; ++k) {
				if (now[k] != a_before[k]) {
					PatchEquipment(k);
				}
			}
		}

		// ---- Equipment: moving the game's key ----

		// The entry of an Equipment slot to make active after a_from (dir +1/-1): carried, and not active in another slot.
		int NextEquipment(int a_slot, int a_from, int a_dir)
		{
			const Slot& slot = g_wheels[kEquip][a_slot];
			const int n = static_cast<int>(slot.entries.size());
			const auto keys = inventory::Keys();
			for (int step = 1; step <= n; ++step) {
				const int i = ((a_from + a_dir * step) % n + n) % n;
				const auto id = slot.entries[i];
				const bool elsewhere = std::find(keys.begin(), keys.end(), id) != keys.end() && keys[a_slot] != id;
				if (inventory::Has(id) && !elsewhere) {
					return i;
				}
			}
			return -1;
		}

		// Takes entry a_index out of a slot; on the Equipment wheel the key goes to the entry that takes its place.
		void RemoveAt(int a_wheel, int a_slot, int a_index, const std::string& a_why)
		{
			Slot& slot = g_wheels[a_wheel][a_slot];
			if (a_index < 0 || a_index >= static_cast<int>(slot.entries.size())) {
				return;
			}
			const auto gone = slot.entries[a_index];
			const bool wasActive = a_index == slot.active;
			slot.entries.erase(slot.entries.begin() + a_index);
			if (slot.active > a_index) {
				--slot.active;
			}
			if (a_wheel == kMagic) {
				if (wasActive) {
					slot.active = slot.entries.empty() ? -1 : std::min(a_index, static_cast<int>(slot.entries.size()) - 1);
				}
			} else {
				inventory::ClearKey(gone);
				if (wasActive || slot.active < 0) {
					slot.active = -1;
					if (!slot.entries.empty()) {
						const int next = NextEquipment(a_slot, a_index - 1, +1);
						if (next >= 0 && inventory::SetKey(slot.entries[next], a_slot)) {
							slot.active = next;
						}
					}
				} else if (slot.active >= 0) {
					inventory::SetKey(slot.entries[slot.active], a_slot);
				}
			}
			logger::info("wheels: {} - {} REMOVED from {} slot {}; slot now {} entr{} (active: {})", a_why, NameOf(a_wheel, gone),
				a_wheel == kMagic ? "Magic" : "Equipment", a_slot + 1, slot.entries.size(), slot.entries.size() == 1 ? "y" : "ies",
				slot.active >= 0 ? NameOf(a_wheel, slot.entries[slot.active]) : "none");
		}

		// Adds to a slot as its active entry (the Equipment wheel moves the game's key onto it).
		bool AddTo(int a_wheel, int a_slot, std::uint32_t a_id, const char* a_why)
		{
			Slot& slot = g_wheels[a_wheel][a_slot];
			if (static_cast<int>(slot.entries.size()) >= Cap()) {
				logger::info("wheels: {} slot {} is full ({} entries) - {} not added", a_wheel == kMagic ? "Magic" : "Equipment", a_slot + 1, Cap(),
					NameOf(a_wheel, a_id));
				return false;
			}
			if (a_wheel == kEquip && !inventory::SetKey(a_id, a_slot)) {
				return false;
			}
			slot.entries.push_back(a_id);
			slot.active = static_cast<int>(slot.entries.size()) - 1;
			logger::info("wheels: {} - {} ADDED to {} slot {}; slot now {} entr{}, it is the active one", a_why, NameOf(a_wheel, a_id),
				a_wheel == kMagic ? "Magic" : "Equipment", a_slot + 1, slot.entries.size(), slot.entries.size() == 1 ? "y" : "ies");
			return true;
		}

		// ---- a pending Equipment assign (the game handles the press between our reads) ----

		struct Pending
		{
			bool              on = false;
			int               slot = -1;
			std::uint32_t     parked = 0;       // the pointed slot's item, taken off its key before the game's assign
			inventory::KeyMap before{};
			int               reads = 0;
			int               readsSinceRelease = 0;
			bool              released = false;
		} g_pending;

		void Settle(int a_slot, std::uint32_t a_arrived)
		{
			const Pending p = g_pending;
			g_pending = {};
			inventory::KeyMap original = p.before;
			if (p.slot >= 0) {
				original[p.slot] = p.parked;
			}
			Slot& slot = g_wheels[kEquip][a_slot];
			const std::uint32_t previous = a_slot == p.slot ? p.parked : p.before[a_slot];   // what the slot held
			if (a_slot != p.slot && p.parked) {
				logger::info("wheels: the game assigned to slot {}, not the pointed slot {}", a_slot + 1, p.slot + 1);
				inventory::SetKey(p.parked, p.slot);   // the pointed slot keeps its item
			}
			// an item the game MOVED here from another slot leaves that slot
			for (int u = 0; u < inventory::kSlots; ++u) {
				if (u != a_slot && p.before[u] == a_arrived) {
					const int idx = IndexOf(g_wheels[kEquip][u], a_arrived);
					if (idx >= 0) {
						g_wheels[kEquip][u].active = idx;
						RemoveAt(kEquip, u, idx, std::format("moved to slot {}", a_slot + 1));
						inventory::SetKey(a_arrived, a_slot);   // RemoveAt cleared it
					}
				}
			}
			const int idx = IndexOf(slot, a_arrived);
			if (a_arrived == previous) {
				// pressed again on the item the slot shows: out it goes, the next entry takes its place
				RemoveAt(kEquip, a_slot, idx >= 0 ? idx : slot.active, "assign pressed again on the same item");
			} else if (idx >= 0) {
				// pressed on an entry the slot holds but is not showing: out it goes, the shown one stays
				slot.entries.erase(slot.entries.begin() + idx);
				inventory::ClearKey(a_arrived);
				slot.active = previous ? IndexOf(slot, previous) : -1;
				if (previous) {
					inventory::SetKey(previous, a_slot);
				}
				logger::info("wheels: assign on {} (already in slot {}) - REMOVED; {} stays active", inventory::NameOf(a_arrived), a_slot + 1,
					previous ? inventory::NameOf(previous) : "nothing");
			} else if (static_cast<int>(slot.entries.size()) >= Cap()) {
				inventory::ClearKey(a_arrived);
				if (previous) {
					inventory::SetKey(previous, a_slot);
				}
				logger::info("wheels: Equipment slot {} is full ({} entries) - {} not added", a_slot + 1, Cap(), inventory::NameOf(a_arrived));
			} else {
				slot.entries.push_back(a_arrived);
				slot.active = static_cast<int>(slot.entries.size()) - 1;
				logger::info("wheels: {} ADDED to Equipment slot {} - now {} entr{}, it is the active one (the others stay in the slot)",
					inventory::NameOf(a_arrived), a_slot + 1, slot.entries.size(), slot.entries.size() == 1 ? "y" : "ies");
			}
			Save();
			PatchChanged(original);
		}

		void SettleNothing()
		{
			const Pending p = g_pending;
			g_pending = {};
			if (p.parked) {
				inventory::SetKey(p.parked, p.slot);
			}
			logger::info("wheels: the assign changed nothing (slot {}) - left as it was", p.slot + 1);
		}

		// Y: on its wheel (the first slot with room) or off it (wherever it is)
		void ToggleFavourite(int a_wheel, std::uint32_t a_id)
		{
			const auto before = inventory::Keys();
			for (int s = 0; s < inventory::kSlots; ++s) {
				const int idx = IndexOf(g_wheels[a_wheel][s], a_id);
				if (idx >= 0) {
					RemoveAt(a_wheel, s, idx, "Favourite toggled off");
					Save();
					PatchChanged(before);
					Refresh();
					return;
				}
			}
			// an empty slot first, then any slot with room
			for (int pass = 0; pass < 2; ++pass) {
				for (int s = 0; s < inventory::kSlots; ++s) {
					const Slot& slot = g_wheels[a_wheel][s];
					const bool fits = pass == 0 ? slot.entries.empty() : static_cast<int>(slot.entries.size()) < Cap();
					if (fits && AddTo(a_wheel, s, a_id, "Favourite")) {
						Save();
						PatchChanged(before);
						Refresh();
						return;
					}
				}
			}
			logger::info("wheels: Favourite - every {} slot is full", a_wheel == kMagic ? "Magic" : "Equipment");
		}
	}

	const char* Name(Wheel a_wheel)
	{
		return a_wheel == Wheel::kMagic ? "Magic" : "Equipment";
	}

	Wheel Active()
	{
		return static_cast<Wheel>(g_active.load());
	}

	bool IsFavourite(std::uint32_t a_formID)
	{
		if (!a_formID || !EnsureLoaded()) {
			return false;
		}
		for (const Slot& slot : g_wheels[kEquip]) {
			if (IndexOf(slot, a_formID) >= 0) {
				return true;
			}
		}
		return false;
	}

	void Favourite(menus::Menu a_menu)
	{
		if (!EnsureLoaded()) {
			return;
		}
		if (a_menu == menus::Menu::kMagic) {
			if (const auto spell = rows::HighlightedSpell()) {
				ToggleFavourite(kMagic, spell);
			}
		} else if (a_menu == menus::Menu::kInventory) {
			if (const auto item = rows::HighlightedItem()) {
				ToggleFavourite(kEquip, item);
			}
		}
	}

	void AssignPressed(menus::Menu a_menu, int a_slot)
	{
		if (!EnsureLoaded()) {
			return;
		}
		if (a_menu == menus::Menu::kMagic) {
			// the Magic wheel's assign is ours (the game never sees A here)
			if (a_slot < 0) {
				logger::info("wheels: assign in the magic menu with no slot pointed - nothing to do");
				return;
			}
			const auto spell = rows::HighlightedSpell();
			if (!spell) {
				return;
			}
			Slot& slot = g_wheels[kMagic][a_slot];
			if (const int idx = IndexOf(slot, spell); idx >= 0) {
				RemoveAt(kMagic, a_slot, idx, "assign pressed on a spell already in the slot");
			} else {
				AddTo(kMagic, a_slot, spell, "assign");
			}
			Save();
			Refresh();
			return;
		}
		if (a_menu != menus::Menu::kInventory) {
			return;
		}
		if (g_pending.on) {
			SettleNothing();
		}
		g_pending = {};
		g_pending.on = true;
		g_pending.slot = a_slot;
		g_pending.before = inventory::Keys();
		if (a_slot >= 0 && g_pending.before[a_slot]) {
			g_pending.parked = g_pending.before[a_slot];
			inventory::ClearKey(g_pending.parked);   // off the key while the game assigns: whatever comes back is what was pressed
			g_pending.before[a_slot] = 0;
		}
		logger::info("wheels: assign pressed with Equipment slot {} pointed ({} taken off the key while the game assigns)", a_slot + 1,
			g_pending.parked ? inventory::NameOf(g_pending.parked) : "nothing");
	}

	void Tick(bool a_assignHeld)
	{
		AssertMagic();
		if (!g_pending.on) {
			return;
		}
		++g_pending.reads;
		if (!a_assignHeld) {
			g_pending.released = true;
		}
		if (g_pending.released) {
			++g_pending.readsSinceRelease;
		}
		const auto after = inventory::Keys();
		for (int s = 0; s < inventory::kSlots; ++s) {
			if (after[s] && after[s] != g_pending.before[s]) {
				Settle(s, after[s]);
				return;
			}
		}
		if (g_pending.readsSinceRelease >= 20 || g_pending.reads >= 300) {
			SettleNothing();
		}
	}

	void CycleEntry(Wheel a_wheel, int a_slot, int a_dir)
	{
		if (a_slot < 0) {
			logger::info("wheels: CYCLE with no slot pointed - nothing to cycle");
			return;
		}
		if (!EnsureLoaded()) {
			return;
		}
		const int w = a_wheel == Wheel::kMagic ? kMagic : kEquip;
		Slot& slot = g_wheels[w][a_slot];
		const int n = static_cast<int>(slot.entries.size());
		int next = -1;
		if (w == kMagic) {
			next = n > 1 ? ((slot.active < 0 ? 0 : slot.active) + a_dir + n) % n : -1;
		} else if (n > 0) {
			next = NextEquipment(a_slot, slot.active < 0 ? (a_dir > 0 ? -1 : 0) : slot.active, a_dir);
		}
		if (next < 0 || next == slot.active) {
			logger::info("wheels: CYCLE {} slot {} - {} entr{}, nothing to step to", Name(a_wheel), a_slot + 1, n, n == 1 ? "y" : "ies");
			return;
		}
		if (w == kEquip && !inventory::SetKey(slot.entries[next], a_slot)) {
			return;
		}
		slot.active = next;
		Save();
		logger::info("wheels: CYCLE {} slot {} -> {} ({} of {})", Name(a_wheel), a_slot + 1, NameOf(w, slot.entries[next]), next + 1, n);
		if (w == kEquip) {
			PatchEquipment(a_slot);
		}
		Refresh();
	}

	void SwitchWheel(int a_dir)
	{
		const Wheel next = Active() == Wheel::kEquipment ? Wheel::kMagic : Wheel::kEquipment;   // two wheels: either way flips
		g_active.store(static_cast<int>(next));
		logger::info("wheels: SWITCH {} -> the {} wheel is active", a_dir > 0 ? "right" : "left", Name(next));
		if (quickkeys::RadialOpen() && EnsureLoaded()) {
			ShowMagic(next == Wheel::kMagic);
		}
	}

	void RemoveEntry(Wheel a_wheel, int a_slot)
	{
		if (a_slot < 0 || !EnsureLoaded()) {
			return;
		}
		const int w = a_wheel == Wheel::kMagic ? kMagic : kEquip;
		Slot& slot = g_wheels[w][a_slot];
		if (slot.active < 0) {
			logger::info("wheels: REMOVE - {} slot {} is empty", Name(a_wheel), a_slot + 1);
			return;
		}
		RemoveAt(w, a_slot, slot.active, "LB on the radial");
		Save();
		if (w == kEquip) {
			PatchEquipment(a_slot);
		}
		Refresh();
	}

	void UseNow(int a_slot)
	{
		logger::info("wheels: USE {} slot {} now (RB) - the radial closes on it", Name(Active()), a_slot + 1);
	}

	void UseMagic(int a_slot)
	{
		if (a_slot < 0) {
			logger::info("wheels: the Magic wheel closed on no slot - nothing cast-ready changed");
			return;
		}
		if (!EnsureLoaded()) {
			return;
		}
		const Slot& slot = g_wheels[kMagic][a_slot];
		if (slot.active < 0) {
			logger::info("wheels: Magic slot {} is empty", a_slot + 1);
			return;
		}
		auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(slot.entries[slot.active]);
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!spell || !player) {
			logger::warn("wheels: Magic slot {} holds 0x{:08X}, which is not a spell now", a_slot + 1, slot.entries[slot.active]);
			return;
		}
		player->SetCurrentSpell(static_cast<RE::MagicItem*>(spell));
		logger::info("wheels: Magic slot {} used - {} is now the spell cast (selected spell reads {})", a_slot + 1, rows::SpellName(spell->GetFormID()),
			player->selectedSpell == static_cast<RE::MagicItem*>(spell) ? "it" : "something else");
	}

	void PanelShown(menus::Menu a_menu)
	{
		if (!EnsureLoaded()) {
			return;
		}
		ShowMagic(a_menu == menus::Menu::kMagic);
	}

	void PanelHidden()
	{
		ShowMagic(false);
	}

	void RadialShown()
	{
		if (EnsureLoaded()) {
			ShowMagic(Active() == Wheel::kMagic);
		}
	}

	void RadialHidden()
	{
		ShowMagic(false);
	}

	std::array<std::string, 8> Describe(Wheel a_wheel)
	{
		std::array<std::string, 8> out;
		if (!EnsureLoaded()) {
			return out;
		}
		const int w = a_wheel == Wheel::kMagic ? kMagic : kEquip;
		for (int s = 0; s < inventory::kSlots; ++s) {
			const Slot& slot = g_wheels[w][s];
			for (int i = 0; i < static_cast<int>(slot.entries.size()); ++i) {
				out[s] += (i ? ", " : "") + NameOf(w, slot.entries[i]) + (i == slot.active ? "*" : "");
			}
		}
		return out;
	}

	std::string Status()
	{
		int equip = 0, magic = 0;
		for (int s = 0; s < inventory::kSlots; ++s) {
			equip += static_cast<int>(g_wheels[kEquip][s].entries.size());
			magic += static_cast<int>(g_wheels[kMagic][s].entries.size());
		}
		return std::format("character {}; Equipment {} entries, Magic {} entries; pictures show {}", g_loadedFor.empty() ? "-" : g_loadedFor, equip,
			magic, g_shown == Shown::kGame ? "the game's keys" : g_shown == Shown::kMagic ? "the Magic wheel" : "the Equipment wheel");
	}
}
