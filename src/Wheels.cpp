#include "Wheels.h"

#include "Inventory.h"
#include "PEHook.h"
#include "Reflect.h"
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
		constexpr int kAmmo = 2;   // the bow's ammo wheel: one arrow kind a slot, entirely ours
		constexpr int kWheels = 3;
		constexpr const char* kWheelNames[kWheels] = { "Equipment", "Magic", "Ammo" };

		std::atomic<int> g_active{ static_cast<int>(Wheel::kEquipment) };

		struct Slot
		{
			std::vector<std::uint32_t> entries;   // formIDs, in the order they were added
			int active = -1;                      // index into entries, -1 = none
		};
		using WheelSlots = std::array<Slot, inventory::kSlots>;
		std::array<WheelSlots, kWheels> g_wheels;
		std::string g_loadedFor;   // the character g_wheels belongs to

		int MigrateAmmo();

		// what the wheel's pictures are showing now
		enum class Shown { kGame, kMagic };
		Shown g_shown = Shown::kGame;

		const char* WheelName(int a_wheel) { return a_wheel >= 0 && a_wheel < kWheels ? kWheelNames[a_wheel] : "?"; }

		bool IsAmmo(std::uint32_t a_id)
		{
			auto* form = a_id ? RE::TESForm::LookupByID(a_id) : nullptr;
			return form && form->GetFormType() == RE::FormType::Ammo;
		}

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

		std::atomic<std::uint32_t> g_generation{ 1 };   // moves with every saved change (the favourites column redraws)

		void Save()
		{
			g_generation.fetch_add(1, std::memory_order_relaxed);
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
			for (int w = 0; w < kWheels; ++w) {
				for (int s = 0; s < inventory::kSlots; ++s) {
					const Slot& slot = g_wheels[w][s];
					if (slot.entries.empty()) {
						continue;
					}
					f << WheelName(w) << ' ' << (s + 1) << " active=" << (slot.active + 1);
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
			g_generation.fetch_add(1, std::memory_order_relaxed);
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
				Slot& slot = g_wheels[wheel == "Magic" ? kMagic : wheel == "Ammo" ? kAmmo : kEquip][slotNo - 1];
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
			MigrateAmmo();
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
		// own eight pictures untouched, so it is always the truth for "the game's keys". A slot is drawn only when it
		// must change: every SetQuickKeyByIndex is the widget's "slot changed" and clicks (the owner heard a stream of
		// clicks while an earlier build redrew all eight every 250 ms). After the game draws its own (UpdateIcons),
		// the widgets show the view model's pictures again, so only the slots that differ from those are redrawn.
		quickkeys::Icons g_shownNow{};       // what the widgets show, as far as we know
		bool             g_shownKnown = false;

		void Draw(const quickkeys::Icons& a_icons, bool /*a_force*/)
		{
			if (quickkeys::TakeGameDrew() || !g_shownKnown) {
				g_shownNow = quickkeys::ReadIcons();
				g_shownKnown = true;
			}
			if (a_icons == g_shownNow) {
				return;
			}
			quickkeys::DrawIcons(a_icons, g_shownNow);
			g_shownNow = a_icons;
		}

		// The game refreshes its pictures only when IT assigns a key, so a slot whose key we moved shows the old or no
		// picture (read 2026-09-29). While the wheel is on screen, each Equipment slot holding an item whose picture a
		// row has shown is drawn with that picture; every other slot keeps the game's.
		std::chrono::steady_clock::time_point g_nextEquipCheck{};

		// The inventory wheel's slots are drawn from the keys on the wheel's own material instance now (MagicWheel.cpp,
		// the equipment pass): the game's picture list (the view model's Icons) was found EMPTY while the keys held items
		// (2026-09-30 04:37, the primary's read), so the game set every slot's opacity to 0, and the old redraw here -
		// SetQuickKeyByIndex, one "slot changed" click per slot - only swapped pictures in and never made them visible.
		constexpr bool kRedrawThroughTheWidget = false;

		void AssertEquipment()
		{
			if (!kRedrawThroughTheWidget || !(quickkeys::RadialOpen() || quickkeys::PanelOpen())) {
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
			if (ours) {
				Draw(icons, false);
			}
		}

		// The Magic wheel is a wheel of its own (the owner, 2026-09-30: "the magic menu wheel is a distinct separate wheel
		// from the inventory wheel. And just like the inventory wheel, even if it's empty, it's still visible and it still
		// draws. It would just be empty"). The widget's own SetQuickKeyByIndex keeps the old picture when given none, so an
		// empty Magic slot went on showing the inventory wheel's item. The Magic wheel is therefore written into the wheel's
		// VIEW MODEL (SetIcons), where the game's own drawing shows an empty key as empty; the game's eight pictures are kept
		// and put back when the Magic wheel leaves the screen.
		quickkeys::Icons       g_gameIcons{};
		bool                   g_haveGameIcons = false;
		std::chrono::steady_clock::time_point g_nextMagicCheck{};

		// The Magic wheel is its own widget now (MagicWheel.cpp): nothing is written into the game's view model - every
		// slot the old SetIcons redrew was the widget's own "slot changed" and ticked (the owner, 2026-09-30: "only on the
		// magic wheel do I hear a bunch of ticking noises as soon as it appears"), and the magic menu still showed the
		// inventory wheel's items ("when I go to inventory and magic, I still see all the same items").
		void WriteMagic(bool /*a_force*/) {}

		void AssertMagic()
		{
			if (g_shown != Shown::kMagic) {
				AssertEquipment();
				return;
			}
			WriteMagic(false);
		}

		void ShowMagic(bool a_on)
		{
			if ((g_shown == Shown::kMagic) == a_on) {
				return;
			}
			if (a_on) {
				g_shown = Shown::kMagic;
				int drawn = 0, filled = 0;
				for (const auto* i : MagicIcons()) {
					drawn += i ? 1 : 0;
				}
				for (const Slot& slot : g_wheels[kMagic]) {
					filled += slot.active >= 0 ? 1 : 0;
				}
				logger::info("wheels: the Magic wheel is up - {} of 8 slots hold a spell, {} with its picture known (its own widget draws them)",
					filled, drawn);
			} else {
				g_shown = Shown::kGame;
				g_shownKnown = false;
				logger::info("wheels: the Magic wheel is down - the game's own wheel shows");
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
			if (a_wheel != kEquip) {   // Magic and Ammo are data only
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
				WheelName(a_wheel), a_slot + 1, slot.entries.size(), slot.entries.size() == 1 ? "y" : "ies",
				slot.active >= 0 ? NameOf(a_wheel, slot.entries[slot.active]) : "none");
		}

		// Taking an item off the wheel the normal way unfavourites it (the owner, 2026-09-29: "I just removed the item from
		// the wheel the normal way. Which should be considered going forward."): every OTHER slot of that wheel lets it go
		// too, so a copy hidden among a slot's inactive entries cannot keep it a favourite (the Revealer of Iniquity stayed
		// undroppable as the first of Equipment slot 3's three entries after it was taken off slots 7 and 1).
		void RemoveFromOtherSlots(int a_wheel, int a_keepSlot, std::uint32_t a_id)
		{
			for (int s = 0; s < inventory::kSlots; ++s) {
				if (s == a_keepSlot) {
					continue;
				}
				for (int idx = IndexOf(g_wheels[a_wheel][s], a_id); idx >= 0; idx = IndexOf(g_wheels[a_wheel][s], a_id)) {
					RemoveAt(a_wheel, s, idx, std::format("taken off the wheel from slot {}", a_keepSlot + 1));
				}
			}
		}

		// Adds to a slot as its active entry (the Equipment wheel moves the game's key onto it).
		bool AddTo(int a_wheel, int a_slot, std::uint32_t a_id, const char* a_why)
		{
			Slot& slot = g_wheels[a_wheel][a_slot];
			if (static_cast<int>(slot.entries.size()) >= Cap()) {
				logger::info("wheels: {} slot {} is full ({} entries) - {} not added", WheelName(a_wheel), a_slot + 1, Cap(),
					NameOf(a_wheel, a_id));
				return false;
			}
			if (a_wheel == kEquip && !inventory::SetKey(a_id, a_slot)) {
				return false;
			}
			slot.entries.push_back(a_id);
			slot.active = static_cast<int>(slot.entries.size()) - 1;
			logger::info("wheels: {} - {} ADDED to {} slot {}; slot now {} entr{}, it is the active one", a_why, NameOf(a_wheel, a_id),
				WheelName(a_wheel), a_slot + 1, slot.entries.size(), slot.entries.size() == 1 ? "y" : "ies");
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
				RemoveFromOtherSlots(kEquip, a_slot, a_arrived);
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
				RemoveFromOtherSlots(kEquip, a_slot, a_arrived);
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
			MigrateAmmo();   // arrows the game's own assign put on a key go to the Ammo wheel
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
					RemoveFromOtherSlots(a_wheel, s, a_id);   // off the whole wheel, not only the first slot holding it
					Save();
					PatchChanged(before);
					Refresh();
					return;
				}
			}
			// an empty slot first, then any slot with room (the Ammo wheel: one arrow kind a slot, empty slots only)
			for (int pass = 0; pass < (a_wheel == kAmmo ? 1 : 2); ++pass) {
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
			logger::info("wheels: Favourite - every {} slot is full", WheelName(a_wheel));
		}

		// Arrows belong on the Ammo wheel (the owner, 2026-09-29: "if anything's already on the normal wheel, they would
		// get moved over. And make sure that the normal wheel is refreshed so there's nothing stale left on it."). Every
		// arrow entry on the Equipment wheel leaves it - the game's key with it, so the radial no longer shows it - and
		// goes to the Ammo wheel's first empty slot unless it is there already. Returns how many moved.
		int MigrateAmmo()
		{
			const auto before = inventory::Keys();
			int moved = 0;
			for (int s = 0; s < inventory::kSlots; ++s) {
				for (int i = static_cast<int>(g_wheels[kEquip][s].entries.size()) - 1; i >= 0; --i) {
					if (i >= static_cast<int>(g_wheels[kEquip][s].entries.size())) {
						continue;   // a removal above re-seated the slot
					}
					const auto id = g_wheels[kEquip][s].entries[i];
					if (!IsAmmo(id)) {
						continue;
					}
					RemoveAt(kEquip, s, i, "arrows belong on the Ammo wheel");
					++moved;
					bool there = false;
					for (const Slot& a : g_wheels[kAmmo]) {
						there = there || IndexOf(a, id) >= 0;
					}
					if (there) {
						continue;
					}
					bool placed = false;
					for (int a = 0; a < inventory::kSlots && !placed; ++a) {
						placed = g_wheels[kAmmo][a].entries.empty() && AddTo(kAmmo, a, id, "moved from the Equipment wheel");
					}
					if (!placed) {
						logger::info("wheels: the Ammo wheel is full - {} left the Equipment wheel and is on no wheel now", inventory::NameOf(id));
					}
				}
			}
			if (moved) {
				Save();
				PatchChanged(before);   // the Equipment wheel's pictures follow its keys: nothing stale left on it
				Refresh();
				logger::info("wheels: {} arrow entr{} moved from the Equipment wheel to the Ammo wheel", moved, moved == 1 ? "y" : "ies");
			}
			return moved;
		}
	}

	namespace
	{
		std::chrono::steady_clock::time_point g_hudCheckAt{};
		UE::UObject*                          g_hudIconBefore = nullptr;   // compared only

		// the HUD's spell picture (VHUDMainViewModel.SpellIcon), read to learn whether it follows selectedSpell
		UE::UObject* HudSpellIcon()
		{
			static auto* cls = UE::StaticFindObject<UE::UClass>(nullptr, nullptr, L"/Script/Altar.VHUDMainViewModel");
			if (!cls || !reflect::Ok()) return nullptr;
			const auto off = reflect::Offset(cls, "SpellIcon");
			const auto vms = reflect::Instances(cls);
			return off >= 0 && !vms.empty() ? *reinterpret_cast<UE::UObject* const*>(reinterpret_cast<const std::uint8_t*>(vms.front()) + off) : nullptr;
		}

		void CheckHud()
		{
			if (g_hudCheckAt == std::chrono::steady_clock::time_point{} || std::chrono::steady_clock::now() < g_hudCheckAt) return;
			g_hudCheckAt = {};
			auto* now = HudSpellIcon();
			logger::info("wheels: the HUD's spell picture {} after the Magic wheel's choice ({})", now != g_hudIconBefore ? "CHANGED" : "did NOT change",
				now ? pe::Utf8(now->GetFName().ToString()) : std::string("none"));
		}
	}

	const char* Name(Wheel a_wheel)
	{
		return a_wheel == Wheel::kMagic ? "Magic" : a_wheel == Wheel::kAmmo ? "Ammo" : "Equipment";
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
		for (const int w : { kEquip, kAmmo }) {
			for (const Slot& slot : g_wheels[w]) {
				if (IndexOf(slot, a_formID) >= 0) {
					return true;
				}
			}
		}
		return false;
	}

	std::unordered_set<std::uint32_t> Favourites()
	{
		std::unordered_set<std::uint32_t> out;
		if (!EnsureLoaded()) {
			return out;
		}
		for (const int w : { kEquip, kAmmo }) {
			for (const Slot& slot : g_wheels[w]) {
				out.insert(slot.entries.begin(), slot.entries.end());
			}
		}
		return out;
	}

	std::uint32_t Generation() { return g_generation.load(std::memory_order_relaxed); }

	void ToggleItem(std::uint32_t a_formID)
	{
		if (!a_formID || !EnsureLoaded()) {
			return;
		}
		ToggleFavourite(IsAmmo(a_formID) ? kAmmo : kEquip, a_formID);   // arrows go to the Ammo wheel, as Y
	}

	std::unordered_set<std::uint32_t> MagicFavourites()
	{
		std::unordered_set<std::uint32_t> out;
		if (!EnsureLoaded()) {
			return out;
		}
		for (const Slot& slot : g_wheels[kMagic]) {
			out.insert(slot.entries.begin(), slot.entries.end());
		}
		return out;
	}

	void ToggleSpell(std::uint32_t a_formID)
	{
		if (!a_formID || !EnsureLoaded()) {
			return;
		}
		ToggleFavourite(kMagic, a_formID);
	}

	std::array<std::uint32_t, 8> MagicSlots()
	{
		std::array<std::uint32_t, 8> out{};
		if (!EnsureLoaded()) {
			return out;
		}
		for (int s = 0; s < inventory::kSlots; ++s) {
			const Slot& slot = g_wheels[kMagic][s];
			out[s] = slot.active >= 0 ? slot.entries[slot.active] : 0;
		}
		return out;
	}

	std::array<std::uint32_t, 8> AmmoSlots()
	{
		std::array<std::uint32_t, 8> out{};
		if (!EnsureLoaded()) {
			return out;
		}
		for (int s = 0; s < inventory::kSlots; ++s) {
			const Slot& slot = g_wheels[kAmmo][s];
			const auto id = slot.active >= 0 ? slot.entries[slot.active] : 0;
			out[s] = id && inventory::Has(id) ? id : 0;
		}
		return out;
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
				ToggleFavourite(IsAmmo(item) ? kAmmo : kEquip, item);   // arrows go to the Ammo wheel
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
				RemoveFromOtherSlots(kMagic, a_slot, spell);
			} else {
				// one slot a spell (the owner, 2026-09-30: "if I assign magic to a slot, I shouldn't be able to assign it the
				// same magic to two other slots. Because it's redundant" - as the Equipment wheel already keeps it): a spell
				// assigned here leaves every other slot
				RemoveFromOtherSlots(kMagic, a_slot, spell);
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
		CheckHud();
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
		const int w = a_wheel == Wheel::kMagic ? kMagic : a_wheel == Wheel::kAmmo ? kAmmo : kEquip;
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
		const int w = a_wheel == Wheel::kMagic ? kMagic : a_wheel == Wheel::kAmmo ? kAmmo : kEquip;
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
		// the SELECTED spell - the one the HUD shows and the cast button casts - is PlayerCharacter::selectedSpell (+0x8F0);
		// SetCurrentSpell alone set the caster's current spell and left the HUD on the old one (the owner, 2026-09-30: "I
		// selected a magic from the magic wheel, but the characters UI widget for their quick magic didn't change"). No
		// reflected equip function exists (the primary's search): the field is set, and the HUD is read back half a second
		// later to learn whether it follows by itself.
		auto* before = player->selectedSpell;
		player->selectedSpell = static_cast<RE::MagicItem*>(spell);
		player->SetCurrentSpell(static_cast<RE::MagicItem*>(spell));
		logger::info("wheels: Magic slot {} used - {} is now the selected spell ({})", a_slot + 1, rows::SpellName(spell->GetFormID()),
			before == static_cast<RE::MagicItem*>(spell) ? "it was already" : before ? "another spell was" : "none was");
		g_hudCheckAt = std::chrono::steady_clock::now() + 500ms;
		g_hudIconBefore = HudSpellIcon();
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
		const int w = a_wheel == Wheel::kMagic ? kMagic : a_wheel == Wheel::kAmmo ? kAmmo : kEquip;
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
		int equip = 0, magic = 0, ammo = 0;
		for (int s = 0; s < inventory::kSlots; ++s) {
			equip += static_cast<int>(g_wheels[kEquip][s].entries.size());
			magic += static_cast<int>(g_wheels[kMagic][s].entries.size());
			ammo += static_cast<int>(g_wheels[kAmmo][s].entries.size());
		}
		return std::format("character {}; Equipment {} entries, Magic {} entries, Ammo {} entries; pictures show {}", g_loadedFor.empty() ? "-" : g_loadedFor, equip,
			magic, ammo, g_shown == Shown::kGame ? "the game's keys" : g_shown == Shown::kMagic ? "the Magic wheel" : "the Equipment wheel");
	}
}
