#include "Wheels.h"

#include "Inventory.h"
#include "PEHook.h"
#include "Reflect.h"
#include "QuickKeys.h"
#include "Rows.h"
#include "Settings.h"
#include "Ui.h"

#include <fstream>
#include <unordered_map>
#include <sstream>

namespace wheels
{
	namespace
	{
		constexpr int kEquip = 0;
		constexpr int kMagic = 1;
		constexpr int kAmmo = 2;   // the bow's ammo wheel: one arrow kind a slot, entirely ours
		constexpr int kFav = 3;    // favourites that go on no wheel: armour and clothing (the owner, 2026-09-30: "It should be fine
		                           // to favorite armor, just not added to the wheel") - all in slot 1, no cap
		constexpr int kWheels = 4;
		constexpr const char* kWheelNames[kWheels] = { "Equipment", "Magic", "Ammo", "Favourite" };

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

		// Armour and clothing never go on the wheel - shields do (the owner, 2026-09-30: "I don't want armor that's favorited
		// to appear on the wheel at all. I just want the shields to be appearing on it"). Oblivion's body slot 13 is the shield.
		constexpr std::uint16_t kShieldSlot = 1u << 13;

		bool Wearable(std::uint32_t a_id, bool* a_shield = nullptr)
		{
			auto* form = a_id ? RE::TESForm::LookupByID(a_id) : nullptr;
			if (!form) return false;
			const RE::TESBipedModelForm* biped = nullptr;
			if (form->GetFormType() == RE::FormType::Armor) biped = form->As<RE::TESObjectARMO>();
			else if (form->GetFormType() == RE::FormType::Clothing) biped = form->As<RE::TESObjectCLOT>();
			if (!biped) return false;
			if (a_shield) *a_shield = (biped->bipedModelData.bipedObjectSlots & kShieldSlot) != 0;
			return true;
		}

		bool AllowedOnEquipment(std::uint32_t a_id)
		{
			bool shield = false;
			return !Wearable(a_id, &shield) || shield;
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

		// Items the player took off the Equipment wheel this session, and the slot each left (cleared with a new character):
		// a game key that comes back on one of them (a loaded save's own keys) is taken off it again, not picked up. Putting
		// it back on the wheel (Y, the watched assign) takes it off this list. (A mouse assign in the inventory is not
		// watched: on an item taken off this session it would be undone - the owner plays on the controller.)
		std::unordered_map<std::uint32_t, int> g_takenOff;

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
			g_takenOff.clear();
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
				Slot& slot = g_wheels[wheel == "Magic" ? kMagic : wheel == "Ammo" ? kAmmo : wheel == "Favourite" ? kFav : kEquip][slotNo - 1];
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

		bool AssignPending();   // the game's own assign is being watched (Settle reads the keys' change itself)


		std::string Unreachable(int a_slot, std::uint32_t a_id, const inventory::KeyMap& a_keys);

		int SlotHolding(std::uint32_t a_id)
		{
			for (int s = 0; s < inventory::kSlots; ++s) {
				if (IndexOf(g_wheels[kEquip][s], a_id) >= 0) {
					return s;
				}
			}
			return -1;
		}

		// The Equipment wheel and the game's keys made to agree - an item is on ONE slot, and each slot's active entry is
		// the item on its key. The WHEEL wins for every item it holds: on 2026-10-01 the owner stacked the longsword and the
		// mace on slot 2 (02:35:18 / 02:35:21; the keys followed, and LT / RT cycled all three), then a save loaded at about
		// 02:40-02:42 put the save's own keys back (mace on 1, longsword on 3), and at 02:46:16 - the first wheel read after
		// it, the radial opening - the old rule ("the game's keys are the truth") took both off slot 2. Now:
		//   * a key on an item the wheel holds on ANOTHER slot comes off that item; the slot's own active entry goes back on
		//     its key;
		//   * a key on an item the player took off the wheel this session comes off it again;
		//   * a key on an item the wheel does not hold at all is still picked up (a key the game set that we never saw);
		//   * a slot whose key holds nothing gets its active entry back on the key; a key on another entry of the same slot
		//     is moved back to the active one.
		// (The 2026-10-01 01:50 case - a wheel file written beside another save - is settled the same way: the file wins.)
		void Reconcile()
		{
			auto keys = inventory::Keys();
			bool changed = false, cleared = false;
			for (int s = 0; s < inventory::kSlots; ++s) {
				const auto id = keys[s];
				Slot& slot = g_wheels[kEquip][s];
				if (!id || IndexOf(slot, id) >= 0) {
					continue;
				}
				if (const int home = SlotHolding(id); home >= 0) {
					inventory::ClearKey(id);
					cleared = true;
					logger::info("wheels: the game's key {} held {}, which the wheel has on slot {} - the key taken off it (the wheel keeps the player's choice; a loaded save brings back its own keys)",
						s + 1, inventory::NameOf(id), home + 1);
					continue;
				}
				if (const auto it = g_takenOff.find(id); it != g_takenOff.end()) {
					inventory::ClearKey(id);
					cleared = true;
					logger::info("wheels: the game's key {} held {}, which was taken off Equipment slot {} this session - the key taken off it again",
						s + 1, inventory::NameOf(id), it->second + 1);
					continue;
				}
				if (static_cast<int>(slot.entries.size()) >= Cap()) {
					logger::info("wheels: Equipment slot {} is full - {} leaves it to make room for the game's key", s + 1, inventory::NameOf(slot.entries.back()));
					slot.entries.pop_back();
					if (slot.active >= static_cast<int>(slot.entries.size())) {
						slot.active = -1;
					}
				}
				slot.entries.push_back(id);
				slot.active = static_cast<int>(slot.entries.size()) - 1;
				g_takenOff.erase(id);
				changed = true;
				logger::info("wheels: Equipment slot {} picked up {} from the game (the item on the game's key {}, on no slot of the wheel)", s + 1,
					inventory::NameOf(id), s + 1);
			}
			if (cleared) {
				keys = inventory::Keys();
			}
			// a keyed item leaves every OTHER slot's entries (an item is on one slot)
			for (int s = 0; s < inventory::kSlots; ++s) {
				if (!keys[s]) {
					continue;
				}
				for (int t = 0; t < inventory::kSlots; ++t) {
					Slot& other = g_wheels[kEquip][t];
					for (int idx = t == s ? -1 : IndexOf(other, keys[s]); idx >= 0; idx = IndexOf(other, keys[s])) {
						other.entries.erase(other.entries.begin() + idx);
						if (other.active == idx) {
							other.active = -1;
						} else if (other.active > idx) {
							--other.active;
						}
						changed = true;
						logger::info("wheels: {} left Equipment slot {}'s entries - it is on slot {}'s key, and an item is on one slot",
							inventory::NameOf(keys[s]), t + 1, s + 1);
					}
				}
			}
			// each slot's active entry on its key
			for (int s = 0; s < inventory::kSlots; ++s) {
				Slot& slot = g_wheels[kEquip][s];
				const int keyed = keys[s] ? IndexOf(slot, keys[s]) : -1;
				const int want = slot.active >= 0 && slot.active < static_cast<int>(slot.entries.size()) ? slot.active : -1;
				if (want >= 0 && want != keyed && Unreachable(s, slot.entries[want], keys).empty() && inventory::SetKey(slot.entries[want], s)) {
					logger::info("wheels: Equipment slot {}'s choice {} put back on the game's key {} (it held {})", s + 1, inventory::NameOf(slot.entries[want]),
						s + 1, keys[s] ? inventory::NameOf(keys[s]) : std::string("nothing"));
					keys[s] = slot.entries[want];
					continue;
				}
				slot.active = keyed;
			}
			if (changed) {
				Save();
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
			// not while the game's own assign is watched: Settle reads what arrived on the key and decides ADDED / REMOVED /
			// moved itself - reconciling first (the favourites column and the slot counters ask every 100 ms) took the
			// arrival as already "in the slot"
			if (!AssignPending()) {
				Reconcile();
			}
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

		// Why an Equipment entry cannot be stepped to (LT / RT, D-pad left / right): it is not carried, or it is on another
		// slot's game key. Empty = it can be.
		std::string Unreachable(int a_slot, std::uint32_t a_id, const inventory::KeyMap& a_keys)
		{
			if (!inventory::Has(a_id)) {
				return "not carried";
			}
			for (int k = 0; k < inventory::kSlots; ++k) {
				if (k != a_slot && a_keys[k] == a_id) {
					return std::format("on slot {}'s key", k + 1);
				}
			}
			return {};
		}

		// The entry of an Equipment slot to make active after a_from (dir +1/-1): carried, and not active in another slot.
		// Every entry passed over is named with its reason in *a_skipped.
		int NextEquipment(int a_slot, int a_from, int a_dir, std::string* a_skipped = nullptr)
		{
			const Slot& slot = g_wheels[kEquip][a_slot];
			const int n = static_cast<int>(slot.entries.size());
			const auto keys = inventory::Keys();
			for (int step = 1; step <= n; ++step) {
				const int i = ((a_from + a_dir * step) % n + n) % n;
				const auto id = slot.entries[i];
				const std::string why = Unreachable(a_slot, id, keys);
				if (why.empty()) {
					return i;
				}
				if (a_skipped && i != slot.active) {
					*a_skipped += std::format("{}{} skipped: {}", a_skipped->empty() ? "" : "; ", inventory::NameOf(id), why);
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
				g_takenOff[gone] = a_slot;   // put back on another slot by the caller (a move) takes it off this list again
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
			if (a_wheel == kEquip) {
				g_takenOff.erase(a_id);
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

		bool AssignPending() { return g_pending.on; }

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
			if (!AllowedOnEquipment(a_arrived)) {
				inventory::ClearKey(a_arrived);
				if (previous) {
					inventory::SetKey(previous, a_slot);
				}
				logger::info("wheels: the game assigned {} to slot {} - armour and clothing never go on the wheel (shields do); the slot keeps {}",
					inventory::NameOf(a_arrived), a_slot + 1, previous ? inventory::NameOf(previous) : "nothing");
				Save();
				PatchChanged(original);
				return;
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
				g_takenOff[a_arrived] = a_slot;
				slot.active = previous ? IndexOf(slot, previous) : -1;
				if (previous) {
					inventory::SetKey(previous, a_slot);
				}
				logger::info("wheels: assign on {} (already in slot {}) - REMOVED; {} stays active", inventory::NameOf(a_arrived), a_slot + 1,
					previous ? inventory::NameOf(previous) : "nothing");
				RemoveFromOtherSlots(kEquip, a_slot, a_arrived);
			} else if (static_cast<int>(slot.entries.size()) >= Cap()) {
				// full: the new favourite takes the place of the entry the slot shows - the one D-pad left / right stepped to
				// (the owner, 2026-09-30: "D-pad LEFT/RIGHT in the inventory steps through the slot's items to choose which one a
				// new favourite replaces"). The game's key is already on the new item; the one replaced leaves the wheel.
				const int at = slot.active >= 0 && slot.active < static_cast<int>(slot.entries.size()) ? slot.active : 0;
				const auto out = slot.entries[at];
				slot.entries[at] = a_arrived;
				slot.active = at;
				g_takenOff[out] = a_slot;
				g_takenOff.erase(a_arrived);
				logger::info("wheels: Equipment slot {} is full ({} entries) - {} REPLACES {} (entry {})", a_slot + 1, Cap(), inventory::NameOf(a_arrived),
					inventory::NameOf(out), at + 1);
			} else {
				slot.entries.push_back(a_arrived);
				slot.active = static_cast<int>(slot.entries.size()) - 1;
				g_takenOff.erase(a_arrived);
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

		// An item's kind, for putting like with like (the owner, 2026-09-30: "similar types of items stack in the same slot
		// when they're favorited. So that great swords go to great swords, claymores to claymores, maces to maces"): a
		// weapon by its type (blade or blunt, one- or two-handed, staff, bow), armour and clothing by the body slot they
		// fill (the lowest slot bit), anything else by its form type. 0 = not known.
		std::uint32_t KindOf(std::uint32_t a_id)
		{
			auto* form = a_id ? RE::TESForm::LookupByID(a_id) : nullptr;
			if (!form) return 0;
			const auto type = form->GetFormType();
			if (type == RE::FormType::Weapon) {
				auto* weap = form->As<RE::TESObjectWEAP>();
				return weap ? 0x100u + static_cast<std::uint32_t>(weap->data.type) : 0;
			}
			const RE::TESBipedModelForm* biped = nullptr;
			if (type == RE::FormType::Armor) biped = form->As<RE::TESObjectARMO>();
			else if (type == RE::FormType::Clothing) biped = form->As<RE::TESObjectCLOT>();
			if (biped) {
				const std::uint32_t slots = biped->bipedModelData.bipedObjectSlots;
				std::uint32_t low = 0;
				while (low < 16 && !(slots & (1u << low))) ++low;
				return (type == RE::FormType::Armor ? 0x200u : 0x300u) + low;
			}
			return 0x400u + static_cast<std::uint32_t>(type);
		}

		// Y: on its wheel (the first slot with room) or off it (wherever it is)
		void ToggleFavourite(int a_wheel, std::uint32_t a_id)
		{
			// armour and clothing: a favourite, never on the wheel (shields go on it)
			if (a_wheel == kEquip && !AllowedOnEquipment(a_id)) {
				Slot& fav = g_wheels[kFav][0];
				if (const int idx = IndexOf(fav, a_id); idx >= 0) {
					fav.entries.erase(fav.entries.begin() + idx);
					logger::info("wheels: Favourite toggled off - {} (armour or clothing: a favourite, not on the wheel)", inventory::NameOf(a_id));
				} else {
					fav.entries.push_back(a_id);
					logger::info("wheels: Favourite - {} is a favourite now (armour or clothing: not put on the wheel; shields go on it)",
						inventory::NameOf(a_id));
				}
				fav.active = fav.entries.empty() ? -1 : 0;
				Save();
				return;
			}
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

			// the Equipment wheel: like with like first - a slot already holding this kind of item, with room
			if (a_wheel == kEquip) {
				if (const auto kind = KindOf(a_id)) {
					for (int s = 0; s < inventory::kSlots; ++s) {
						const Slot& slot = g_wheels[a_wheel][s];
						if (slot.entries.empty() || static_cast<int>(slot.entries.size()) >= Cap()) continue;
						const bool same = std::any_of(slot.entries.begin(), slot.entries.end(), [&](std::uint32_t e) { return KindOf(e) == kind; });
						if (same && AddTo(a_wheel, s, a_id, "Favourite (with its kind)")) {
							Save();
							PatchChanged(before);
							Refresh();
							return;
						}
					}
				}
			}
			// then an empty slot, then any slot with room (the Ammo wheel: one arrow kind a slot, empty slots only)
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
					if (!AllowedOnEquipment(id)) {
						RemoveAt(kEquip, s, i, "armour and clothing never go on the wheel (shields do) - it stays a favourite");
						if (IndexOf(g_wheels[kFav][0], id) < 0) {
							g_wheels[kFav][0].entries.push_back(id);
							g_wheels[kFav][0].active = 0;
						}
						++moved;
						continue;
					}
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
		// RB (or A) on an Equipment slot of the HUD radial: the radial closes on NOTHING (the choice cleared, the stick
		// held centred), and once it has closed the game's own quick-key press for that key is run - the path the number
		// keys 1-8 take (quickkeys::PressQuickKey). The vanilla close did not equip reliably: 2026-10-01 01:52:39, "USE
		// Equipment slot 2 now (RB)" then "the game uses slot 2" (the view model's KeyIndex, our reading), and the Steel
		// Claymore on key 2 never came to hand - the bow stayed. What the use did is read back from the item itself.
		struct PendingUse
		{
			bool                                  on = false;
			bool                                  closed = false;
			bool                                  pressed = false;
			bool                                  direct = false;   // equipped at once through Actor::EquipObject: only the read-back is left
			int                                   key = -1;
			std::uint32_t                         id = 0;
			inventory::State                      before{};
			const char*                           how = "RB";
			std::chrono::steady_clock::time_point armed{}, closedAt{}, verifyAt{};
		} g_use;
		std::string g_lastUse = "none yet";

		bool Equippable(std::uint32_t a_id)
		{
			auto* form = a_id ? RE::TESForm::LookupByID(a_id) : nullptr;
			if (!form) return false;
			const auto t = form->GetFormType();
			return t == RE::FormType::Weapon || t == RE::FormType::Armor || t == RE::FormType::Clothing || t == RE::FormType::Light;
		}

		void DriveUse()
		{
			if (!g_use.on) {
				return;
			}
			using namespace std::chrono;
			const auto now = steady_clock::now();
			const auto name = inventory::NameOf(g_use.id);
			const int  n = g_use.key + 1;
			if (g_use.direct) {
				if (now < g_use.verifyAt) {
					return;
				}
				g_use.on = false;
				const auto st = inventory::StateOf(g_use.id);
				const auto ms = duration_cast<milliseconds>(now - g_use.armed).count();
				g_lastUse = std::format("slot {}: {} {} ({} -> {})", n, name, st.worn ? "equipped at once" : "NOT worn after the equip", inventory::Describe(g_use.before),
					inventory::Describe(st));
				logger::info("wheels: USE result slot {} ({}) - {}: {} -> {} {} ms after the press - {}", n, g_use.how, name, inventory::Describe(g_use.before),
					inventory::Describe(st), ms, st.worn ? "equipped at once through Actor::EquipObject" : "NOT worn: the equip did not take");
				return;
			}
			if (!g_use.pressed) {
				if (quickkeys::RadialOpen()) {
					if (now - g_use.armed > 3s) {
						g_use.on = false;
						g_lastUse = std::format("slot {}: the radial never closed", n);
						logger::info("wheels: USE slot {} - the radial did not close within 3 s; {} not used", n, name);
					}
					return;
				}
				if (!g_use.closed) {
					g_use.closed = true;
					g_use.closedAt = now;
				}
				if (now - g_use.closedAt < 150ms) {
					return;   // the TES side's own close settles first
				}
				if (menus::AnyOpen()) {
					if (now - g_use.closedAt > 1500ms) {
						g_use.on = false;
						g_lastUse = std::format("slot {}: a menu stayed open", n);
						logger::info("wheels: USE slot {} - a menu is open 1.5 s after the radial closed; {} not used", n, name);
					}
					return;
				}
				const auto keys = inventory::Keys();
				const auto st = inventory::StateOf(g_use.id);
				if (keys[static_cast<std::size_t>(g_use.key)] != g_use.id) {
					g_use.on = false;
					g_lastUse = std::format("slot {}: the key moved", n);
					logger::info("wheels: USE slot {} - the game's key {} no longer holds {}; nothing pressed", n, n, name);
					return;
				}
				if (st != g_use.before) {
					// the radial's own close used it after all (the choice was cleared, so it should not have): never twice
					g_use.on = false;
					g_lastUse = std::format("slot {}: {} used by the radial's own close ({} -> {})", n, name, inventory::Describe(g_use.before),
						inventory::Describe(st));
					logger::info("wheels: USE slot {} - the radial's own close already used {} ({} -> {}); the key is not pressed again", n, name,
						inventory::Describe(g_use.before), inventory::Describe(st));
					return;
				}
				const bool ok = quickkeys::PressQuickKey(g_use.key);
				logger::info("wheels: USE slot {} - the game's own quick key {} pressed for {} (VEnhancedAltarPlayerController Quick{}Input_Pressed / _Released){}",
					n, n, name, n, ok ? "" : " - the call FAILED");
				if (!ok) {
					g_use.on = false;
					g_lastUse = std::format("slot {}: the key press could not be made", n);
					return;
				}
				g_use.pressed = true;
				g_use.verifyAt = now + 700ms;
				return;
			}
			if (now < g_use.verifyAt) {
				return;
			}
			g_use.on = false;
			const auto st = inventory::StateOf(g_use.id);
			const bool menu = menus::AnyOpen();
			if (st != g_use.before || menu) {
				g_lastUse = std::format("slot {}: {} used by the game's key press ({} -> {}{})", n, name, inventory::Describe(g_use.before),
					inventory::Describe(st), menu ? ", a menu opened" : "");
				logger::info("wheels: USE result slot {} ({}) - {}: {} -> {}{} - used through the game's own quick key {}", n, g_use.how, name,
					inventory::Describe(g_use.before), inventory::Describe(st), menu ? ", a menu opened" : "", n);
				return;
			}
			if (Equippable(g_use.id) && st.carried && !st.worn) {
				g_lastUse = std::format("slot {}: {} unchanged after the key press - fallback equip", n, name);
				logger::info("wheels: USE result slot {} ({}) - {} unchanged 0.7 s after the game's key press ({}); FALLBACK: equipped through Actor::EquipObject on the TES thread",
					n, g_use.how, name, inventory::Describe(st));
				inventory::EquipKeyed(g_use.id);
				return;
			}
			g_lastUse = std::format("slot {}: {} unchanged after the key press", n, name);
			logger::info("wheels: USE result slot {} ({}) - {} unchanged 0.7 s after the game's key press ({}) - nothing visible changed", n, g_use.how, name,
				inventory::Describe(st));
		}

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

		// The HUD's spell picture after a Magic wheel choice. Setting selectedSpell does not reach the HUD: the magic menu's
		// own pick makes the game write VHUDMainViewModel.SpellIcon natively and broadcast the field's change, and the HUD's
		// binding then calls GetSpellIcon once (probe B, 2026-10-01: OnItemClicked, then GetSpellIcon x1 and nothing else
		// spell-related on the HUD; every Magic wheel pick logged "did NOT change"). The same, from here: the spell's icon
		// written into SpellIcon, then MVVMViewModelBase's K2_BroadcastFieldValueChanged for "SpellIcon" (the HUD's view
		// models are VViewModelBase -> MVVMViewModelBase).
		void PushHudSpellIcon(std::uint32_t a_formID)
		{
			static auto* cls = UE::StaticFindObject<UE::UClass>(nullptr, nullptr, L"/Script/Altar.VHUDMainViewModel");
			static auto* texCls = ui::Class(L"/Script/Engine.Texture2D");
			if (!cls || !texCls || !reflect::Ok()) {
				logger::warn("wheels: the HUD's spell picture cannot be set (view model class {}, Texture2D {})", cls ? "found" : "missing", texCls ? "found" : "missing");
				return;
			}
			const auto off = reflect::Offset(cls, "SpellIcon");
			const auto vms = reflect::Instances(cls);
			auto*      icon = rows::SpellIcon(a_formID);
			if (off < 0 || vms.empty() || !icon || !icon->GetClass()->IsChildOf(texCls)) {
				logger::info("wheels: the HUD's spell picture left as it is (SpellIcon at {}, {} view model(s), icon {})", off, vms.size(),
					!icon ? std::string("none") : pe::Utf8(icon->GetClass()->GetFName().ToString()));
				return;
			}
			auto* vm = vms.front();
			*reinterpret_cast<UE::UObject**>(reinterpret_cast<std::uint8_t*>(vm) + off) = icon;
			ui::Call broadcast(vm, L"K2_BroadcastFieldValueChanged");
			void* id = broadcast ? broadcast.At("FieldId") : nullptr;
			if (!id) {
				logger::warn("wheels: SpellIcon written, but the view model has no K2_BroadcastFieldValueChanged(FieldId) - the HUD keeps its picture until the game asks again");
				return;
			}
			new (id) UE::FName(L"SpellIcon", UE::EFindName::Add);   // FFieldNotificationId { FName FieldName }
			broadcast.Run();
			logger::info("wheels: the HUD's spell picture set to {} and its change broadcast", pe::Utf8(icon->GetFName().ToString()));
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
		for (const int w : { kEquip, kAmmo, kFav }) {
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
		for (const int w : { kEquip, kAmmo, kFav }) {
			for (const Slot& slot : g_wheels[w]) {
				out.insert(slot.entries.begin(), slot.entries.end());
			}
		}
		return out;
	}

	std::uint32_t Generation() { return g_generation.load(std::memory_order_relaxed); }

	bool CanFavourite(std::uint32_t a_formID)
	{
		return a_formID != 0;   // armour too: a favourite, just never on the wheel
	}

	void ToggleItem(std::uint32_t a_formID)
	{
		if (!a_formID || !EnsureLoaded()) {
			return;
		}
		ToggleFavourite(IsAmmo(a_formID) ? kAmmo : kEquip, a_formID);   // arrows go to the Ammo wheel, as Y
	}

	std::array<SlotCount, 8> SlotCounts(Wheel a_wheel)
	{
		std::array<SlotCount, 8> out{};
		if (!EnsureLoaded()) {
			return out;
		}
		const int w = a_wheel == Wheel::kMagic ? kMagic : a_wheel == Wheel::kAmmo ? kAmmo : kEquip;
		// the Equipment counter counts what LT / RT can show - an entry not carried, or on another slot's key, is not one
		// (the owner, 2026-10-01: slot 2 read 2 while LT / RT had nothing to step to) - and says which of those the slot
		// shows now (the owner, 2026-10-01: "the numbers for which item I'm selecting isn't changing whether I'm on one out
		// of five, two out of five")
		const auto keys = w == kEquip ? inventory::Keys() : inventory::KeyMap{};
		for (int s = 0; s < inventory::kSlots; ++s) {
			const Slot& slot = g_wheels[w][s];
			for (int i = 0; i < static_cast<int>(slot.entries.size()); ++i) {
				if (w == kEquip && !Unreachable(s, slot.entries[i], keys).empty()) {
					continue;
				}
				++out[s].count;
				if (i == slot.active) {
					out[s].position = out[s].count;
				}
			}
		}
		return out;
	}

	int SlotCap() { return Cap(); }

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
		DriveUse();
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
		std::string skipped;
		if (w == kMagic) {
			next = n > 1 ? ((slot.active < 0 ? 0 : slot.active) + a_dir + n) % n : -1;
		} else if (n > 0) {
			next = NextEquipment(a_slot, slot.active < 0 ? (a_dir > 0 ? -1 : 0) : slot.active, a_dir, &skipped);
		}
		if (next < 0 || next == slot.active) {
			logger::info("wheels: CYCLE {} slot {} - {} entr{}, nothing to step to{}", Name(a_wheel), a_slot + 1, n, n == 1 ? "y" : "ies",
				skipped.empty() ? std::string() : " (" + skipped + ")");
			return;
		}
		if (!skipped.empty()) {
			logger::info("wheels: CYCLE {} slot {} - {}", Name(a_wheel), a_slot + 1, skipped);
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

	void UseNow(int a_slot, const char* a_how)
	{
		if (a_slot < 0) {
			logger::info("wheels: USE ({}) with no slot pointed - nothing used", a_how);
			return;
		}
		if (!EnsureLoaded()) {
			return;
		}
		const auto id = inventory::Keys()[static_cast<std::size_t>(a_slot)];
		if (!id) {
			logger::info("wheels: USE Equipment slot {} ({}) - the game's key {} holds nothing; nothing used", a_slot + 1, a_how, a_slot + 1);
			g_lastUse = std::format("slot {}: empty key", a_slot + 1);
			return;
		}
		g_use = {};
		g_use.on = true;
		g_use.key = a_slot;
		g_use.id = id;
		g_use.before = inventory::StateOf(id);
		g_use.how = a_how;
		g_use.armed = std::chrono::steady_clock::now();
		// Equipment - a weapon, a shield, armour, clothing, a torch - goes on AT ONCE through Actor::EquipObject on the TES
		// thread (lock argument false). The game's own quick-key press (Quick<N>Input_Pressed / _Released) equipped nothing
		// in any of the 9 RB / A uses of 2026-10-01 02:32-02:37 ("unchanged 0.7 s after the game's key press ... FALLBACK"),
		// and the EquipObject fallback worked every time ("(x1, worn)") - so the press and its 0.7 s wait are gone for it.
		if (Equippable(id)) {
			if (g_use.before.worn) {
				// what the game's own quick key does with an item already worn (unequip it, or nothing) is not known from the
				// code or the game - the key press itself did nothing at all - so nothing is done, and said
				g_use.on = false;
				g_lastUse = std::format("slot {}: {} already worn - nothing done", a_slot + 1, inventory::NameOf(id));
				logger::info("wheels: USE Equipment slot {} ({}) - {} is already worn ({}); nothing done (what the game's own quick key does with a worn item is not known)",
					a_slot + 1, a_how, inventory::NameOf(id), inventory::Describe(g_use.before));
				return;
			}
			g_use.direct = true;
			g_use.verifyAt = g_use.armed + 300ms;
			inventory::EquipKeyed(id);
			logger::info("wheels: USE Equipment slot {} ({}) - {} ({}): equipped at once through Actor::EquipObject on the TES thread (no game key press, no wait for the close)",
				a_slot + 1, a_how, inventory::NameOf(id), inventory::Describe(g_use.before));
			return;
		}
		// anything else (a potion, a scroll, a book...): the game's own key press once the radial has closed, as before -
		// unproven for these kinds (it never equipped a weapon), and the result is read back and logged
		logger::info("wheels: USE Equipment slot {} ({}) - {} ({}); the radial closes on nothing, then the game's own quick key {} is pressed",
			a_slot + 1, a_how, inventory::NameOf(id), inventory::Describe(g_use.before), a_slot + 1);
	}

	std::string LastUse() { return g_lastUse; }

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
		PushHudSpellIcon(spell->GetFormID());
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
