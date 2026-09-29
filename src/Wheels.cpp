#include "Wheels.h"

#include "Inventory.h"
#include "Settings.h"

#include <fstream>
#include <sstream>

namespace wheels
{
	namespace
	{
		std::atomic<int> g_active{ static_cast<int>(Wheel::kEquipment) };

		struct Slot
		{
			std::vector<std::uint32_t> entries;   // formIDs, in the order they were added
			int active = -1;                      // index into entries, -1 = none
		};
		using WheelSlots = std::array<Slot, inventory::kSlots>;
		std::array<WheelSlots, 2> g_wheels;
		std::string g_loadedFor;   // the character g_wheels belongs to

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
					f << (w == 0 ? "Equipment" : "Magic") << ' ' << (s + 1) << " active=" << (slot.active + 1);
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
				Slot& slot = g_wheels[wheel == "Magic" ? 1 : 0][slotNo - 1];
				std::string id;
				while (in >> id) {
					slot.entries.push_back(static_cast<std::uint32_t>(std::stoul(id, nullptr, 16)));
				}
				slot.active = std::clamp(std::atoi(activeText.c_str() + 7) - 1, -1, static_cast<int>(slot.entries.size()) - 1);
				++rows;
			}
			logger::info("wheels: loaded {} slot(s) for {}", rows, a_name);
		}

		// ---- the Equipment wheel against the game's keys ----

		int IndexOf(const Slot& a_slot, std::uint32_t a_id)
		{
			const auto it = std::find(a_slot.entries.begin(), a_slot.entries.end(), a_id);
			return it == a_slot.entries.end() ? -1 : static_cast<int>(it - a_slot.entries.begin());
		}

		int Cap() { return std::max(1, settings::Get().entriesPerSlot); }

		// The game's keys are the truth for each slot's active entry (the save carries them).
		void Reconcile()
		{
			const auto keys = inventory::Keys();
			for (int s = 0; s < inventory::kSlots; ++s) {
				Slot& slot = g_wheels[0][s];
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
					logger::info("wheels: slot {} picked up {} from the game", s + 1, inventory::NameOf(keys[s]));
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

		// The entry of a slot to make active after a_from (dir +1/-1): carried, and not active in another slot.
		int NextEntry(int a_slot, int a_from, int a_dir)
		{
			const Slot& slot = g_wheels[0][a_slot];
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

		// Takes entry a_index out of slot a_slot; the key goes to the entry that takes its place (or nowhere).
		void RemoveAt(int a_slot, int a_index, const char* a_why)
		{
			Slot& slot = g_wheels[0][a_slot];
			if (a_index < 0 || a_index >= static_cast<int>(slot.entries.size())) {
				return;
			}
			const auto gone = slot.entries[a_index];
			const bool wasActive = a_index == slot.active;
			slot.entries.erase(slot.entries.begin() + a_index);
			if (slot.active > a_index) {
				--slot.active;
			}
			inventory::ClearKey(gone);
			if (wasActive || slot.active < 0) {
				slot.active = -1;
				if (!slot.entries.empty()) {
					const int next = NextEntry(a_slot, a_index - 1, +1);
					if (next >= 0 && inventory::SetKey(slot.entries[next], a_slot)) {
						slot.active = next;
					}
				}
			} else if (slot.active >= 0) {
				inventory::SetKey(slot.entries[slot.active], a_slot);   // the other entry stays on the key
			}
			logger::info("wheels: {} - {} REMOVED from Equipment slot {}; slot now {} entr{} (active: {})", a_why,
				inventory::NameOf(gone), a_slot + 1, slot.entries.size(), slot.entries.size() == 1 ? "y" : "ies",
				slot.active >= 0 ? inventory::NameOf(slot.entries[slot.active]) : "none");
		}

		// ---- a pending assign (the game handles the press between our reads) ----

		struct Pending
		{
			bool             on = false;
			int              slot = -1;
			std::uint32_t    parked = 0;       // the pointed slot's item, taken off its key before the game's assign
			inventory::KeyMap before{};
			int              reads = 0;
			int              readsSinceRelease = 0;
			bool             released = false;
		} g_pending;

		void Settle(int a_slot, std::uint32_t a_arrived)
		{
			const Pending p = g_pending;
			g_pending = {};
			Slot& slot = g_wheels[0][a_slot];
			const std::uint32_t previous = a_slot == p.slot ? p.parked : p.before[a_slot];   // what the slot held
			if (a_slot != p.slot && p.parked) {
				inventory::SetKey(p.parked, p.slot);   // the game put it somewhere else: the pointed slot keeps its item
			}
			// an item the game MOVED here from another slot leaves that slot
			for (int u = 0; u < inventory::kSlots; ++u) {
				if (u != a_slot && p.before[u] == a_arrived) {
					const int idx = IndexOf(g_wheels[0][u], a_arrived);
					if (idx >= 0) {
						g_wheels[0][u].active = idx;   // it was active there
						RemoveAt(u, idx, std::format("moved to slot {}", a_slot + 1).c_str());
						inventory::SetKey(a_arrived, a_slot);   // RemoveAt cleared it
					}
				}
			}
			const int idx = IndexOf(slot, a_arrived);
			if (a_arrived == previous || idx >= 0) {
				// pressed on something the slot already holds: out it goes
				slot.active = idx >= 0 ? idx : slot.active;
				if (idx >= 0 && a_arrived != previous && previous) {
					// an inactive entry: remove it, and the entry that was active goes back on the key
					slot.entries.erase(slot.entries.begin() + idx);
					inventory::ClearKey(a_arrived);
					slot.active = IndexOf(slot, previous);
					inventory::SetKey(previous, a_slot);
					logger::info("wheels: assign on {} (already in slot {}) - REMOVED; {} stays active", inventory::NameOf(a_arrived), a_slot + 1,
						inventory::NameOf(previous));
				} else {
					RemoveAt(a_slot, idx >= 0 ? idx : IndexOf(slot, previous), "assign pressed on the same item");
				}
			} else if (static_cast<int>(slot.entries.size()) >= Cap()) {
				inventory::ClearKey(a_arrived);
				if (previous) {
					inventory::SetKey(previous, a_slot);
				}
				logger::info("wheels: Equipment slot {} is full ({} entries) - {} not added", a_slot + 1, Cap(), inventory::NameOf(a_arrived));
			} else {
				slot.entries.push_back(a_arrived);
				slot.active = static_cast<int>(slot.entries.size()) - 1;
				logger::info("wheels: {} ADDED to Equipment slot {} - now {} entr{}, it is the active one", inventory::NameOf(a_arrived),
					a_slot + 1, slot.entries.size(), slot.entries.size() == 1 ? "y" : "ies");
			}
			Save();
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
	}

	const char* Name(Wheel a_wheel)
	{
		return a_wheel == Wheel::kMagic ? "Magic" : "Equipment";
	}

	Wheel Active()
	{
		return static_cast<Wheel>(g_active.load());
	}

	void Favourite(menus::Menu a_menu)
	{
		const Wheel w = a_menu == menus::Menu::kMagic ? Wheel::kMagic : Wheel::kEquipment;
		logger::info("wheels: FAVOURITE pressed in the {} - toggles the highlighted entry on the {} wheel (the highlighted row is not read yet)",
			menus::Name(a_menu), Name(w));
	}

	void AssignPressed(menus::Menu a_menu, int a_slot)
	{
		if (a_menu != menus::Menu::kInventory) {
			logger::info("wheels: assign in the {} left to the game (the Magic wheel's storage is not found yet)", menus::Name(a_menu));
			return;
		}
		if (g_pending.on) {
			SettleNothing();
		}
		if (!EnsureLoaded()) {
			return;
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
		logger::info("wheels: assign pressed with slot {} pointed ({} taken off the key while the game assigns)", a_slot + 1,
			g_pending.parked ? inventory::NameOf(g_pending.parked) : "nothing");
	}

	void Tick(bool a_assignHeld)
	{
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
		if (a_wheel == Wheel::kMagic) {
			logger::info("wheels: CYCLE on the Magic wheel slot {} - not available yet (spell storage not found)", a_slot + 1);
			return;
		}
		if (a_slot < 0) {
			logger::info("wheels: CYCLE with no slot pointed - nothing to cycle");
			return;
		}
		if (!EnsureLoaded()) {
			return;
		}
		Slot& slot = g_wheels[0][a_slot];
		const int next = slot.entries.empty() ? -1 : NextEntry(a_slot, slot.active < 0 ? (a_dir > 0 ? -1 : 0) : slot.active, a_dir);
		if (next < 0 || next == slot.active) {
			logger::info("wheels: CYCLE Equipment slot {} - {} entr{}, nothing to step to", a_slot + 1, slot.entries.size(),
				slot.entries.size() == 1 ? "y" : "ies");
			return;
		}
		if (inventory::SetKey(slot.entries[next], a_slot)) {
			slot.active = next;
			Save();
			logger::info("wheels: CYCLE Equipment slot {} -> {} ({} of {})", a_slot + 1, inventory::NameOf(slot.entries[next]), next + 1,
				slot.entries.size());
		}
	}

	void SwitchWheel(int a_dir)
	{
		const Wheel next = Active() == Wheel::kEquipment ? Wheel::kMagic : Wheel::kEquipment;   // two wheels: either way flips
		g_active.store(static_cast<int>(next));
		logger::info("wheels: SWITCH {} -> the {} wheel is active (the radial's slots are not rewritten yet)",
			a_dir > 0 ? "right" : "left", Name(next));
	}

	void RemoveEntry(Wheel a_wheel, int a_slot)
	{
		if (a_wheel == Wheel::kMagic) {
			logger::info("wheels: REMOVE on the Magic wheel slot {} - not available yet", a_slot + 1);
			return;
		}
		if (a_slot < 0 || !EnsureLoaded()) {
			return;
		}
		Slot& slot = g_wheels[0][a_slot];
		if (slot.active < 0) {
			logger::info("wheels: REMOVE - Equipment slot {} is empty", a_slot + 1);
			return;
		}
		RemoveAt(a_slot, slot.active, "LB on the radial");
		Save();
	}

	void UseNow(int a_slot)
	{
		logger::info("wheels: USE slot {} now (RB) - the radial closes on it", a_slot + 1);
	}
}
