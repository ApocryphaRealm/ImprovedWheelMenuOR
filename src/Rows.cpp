#include "Rows.h"

#include "PEHook.h"
#include "Inventory.h"
#include "Reflect.h"

#include <unordered_map>
#include <unordered_set>

namespace rows
{
	namespace
	{
		constexpr const wchar_t* kInventoryRowPath = L"/Game/UI/Original/GameMenuLayer/Inventory/MainPart/WBP_OriginalMenu_InventoryEntry.WBP_OriginalMenu_InventoryEntry_C";
		constexpr const wchar_t* kMagicRowPath = L"/Game/UI/Modern/GameMenuLayer/Magic/WBP_ModernMenu_MagicEntry.WBP_ModernMenu_MagicEntry_C";
		constexpr const wchar_t* kInventoryPropsPath = L"/Script/Altar.OriginalInventoryMenuItemProperties";
		constexpr const wchar_t* kMagicPropsPath = L"/Script/Altar.LegacyMagicMenuItemProperties";
		constexpr std::ptrdiff_t kUTESFormID = 0x28;   // UTESForm::m_formID (CommonLibOB64 UTESForm.h)

		struct Kind
		{
			UE::UClass*  rowClass = nullptr;
			std::int32_t properties = -1;   // the row widget's Properties struct
			std::int32_t name = -1, icon = -1, form = -1;
			std::int32_t inContainer = -1, playerItem = -1;   // bIsInContainerMenu, bIsInventoryItem (inventory rows)
			UE::UObject* highlighted = nullptr;
			UE::UFunction* fnSelection = nullptr;   // this row class's BP_OnItemSelectionChanged
			std::unordered_set<UE::UObject*> seen;
		};
		Kind g_inventory, g_magic;

		std::unordered_map<std::uint32_t, UE::UObject*> g_itemIcons, g_spellIcons;
		std::unordered_map<std::string, std::uint32_t> g_spellsByName;
		bool g_dirty = true;   // a row changed since the caches were last filled

		void* PropsOf(Kind& a_kind, UE::UObject* a_row)
		{
			return a_row && a_kind.properties >= 0 && reflect::IsLive(a_row) ? reflect::At<void>(a_row, a_kind.properties) : nullptr;
		}

		std::string NameOfRow(Kind& a_kind, UE::UObject* a_row)
		{
			void* props = PropsOf(a_kind, a_row);
			auto* text = reflect::At<UE::FText>(props, a_kind.name);
			return text ? reflect::Text(*text) : std::string();
		}

		// the name's string-table key: a spell's full name in the game data IS this key ("LOC_FN_...")
		std::string KeyOfRow(Kind& a_kind, UE::UObject* a_row)
		{
			void* props = PropsOf(a_kind, a_row);
			auto* text = reflect::At<UE::FText>(props, a_kind.name);
			return text ? reflect::TextKey(*text) : std::string();
		}

		// An inventory row's item. Its Properties.form is null in the player's inventory (read 2026-09-29: Iron Arrow,
		// bIsInventoryItem true, form null), so the name decides: its string-table key is the item's full name in the
		// game data; a name the player gave (an enchanted item) is not from a table and is matched as it reads.
		std::uint32_t FormOfInventoryRow(UE::UObject* a_row)
		{
			void* props = PropsOf(g_inventory, a_row);
			if (!props) {
				return 0;
			}
			auto** form = reflect::At<UE::UObject*>(props, g_inventory.form);
			if (form && *form && reflect::IsLive(*form)) {
				const auto id = static_cast<std::uint32_t>(*reinterpret_cast<const std::int64_t*>(reinterpret_cast<const std::uint8_t*>(*form) + kUTESFormID));
				if (RE::TESForm::LookupByID(id)) {
					return id;
				}
			}
			const std::string key = KeyOfRow(g_inventory, a_row);
			if (const auto id = inventory::FindByName(key.empty() ? NameOfRow(g_inventory, a_row) : key)) {
				return id;
			}
			return 0;
		}

		UE::UObject* IconOfRow(Kind& a_kind, UE::UObject* a_row)
		{
			void* props = PropsOf(a_kind, a_row);
			auto** icon = reflect::At<UE::UObject*>(props, a_kind.icon);
			return icon ? *icon : nullptr;
		}

		// what every live row shows now goes into the icon caches
		void Harvest()
		{
			if (!g_dirty) {
				return;
			}
			g_dirty = false;
			for (auto it = g_inventory.seen.begin(); it != g_inventory.seen.end();) {
				if (!reflect::IsLive(*it)) {
					it = g_inventory.seen.erase(it);
					continue;
				}
				if (const auto id = FormOfInventoryRow(*it)) {
					if (auto* icon = IconOfRow(g_inventory, *it)) {
						g_itemIcons[id] = icon;
					}
				}
				++it;
			}
			for (auto it = g_magic.seen.begin(); it != g_magic.seen.end();) {
				if (!reflect::IsLive(*it)) {
					it = g_magic.seen.erase(it);
					continue;
				}
				if (const auto id = SpellByName(KeyOfRow(g_magic, *it))) {
					if (auto* icon = IconOfRow(g_magic, *it)) {
						g_spellIcons[id] = icon;
					}
				}
				++it;
			}
		}

		void OnRow(Kind& a_kind, UE::UObject* a_obj, UE::UFunction* a_fn, void* a_params)
		{
			a_kind.seen.insert(a_obj);
			g_dirty = true;
			if (!a_kind.fnSelection && pe::FunctionName(a_fn) == "BP_OnItemSelectionChanged") {
				a_kind.fnSelection = a_fn;
			}
			if (a_fn == a_kind.fnSelection && a_params && *static_cast<const bool*>(a_params)) {
				a_kind.highlighted = a_obj;
			}
		}

		void OnInventoryRow(UE::UObject* a_obj, UE::UFunction* a_fn, void* a_params) { OnRow(g_inventory, a_obj, a_fn, a_params); }
		void OnMagicRow(UE::UObject* a_obj, UE::UFunction* a_fn, void* a_params) { OnRow(g_magic, a_obj, a_fn, a_params); }

		void TryWatch(Kind& a_kind, const wchar_t* a_rowPath, const wchar_t* a_propsPath, pe::Handler a_handler, bool a_hasForm, const char* a_what)
		{
			if (a_kind.rowClass || !reflect::Ok()) {
				return;
			}
			auto* cls = UE::StaticFindObject<UE::UClass>(nullptr, nullptr, a_rowPath);
			auto* props = UE::StaticFindObject<UE::UStruct>(nullptr, nullptr, a_propsPath);
			if (!cls || !props) {
				return;
			}
			a_kind.properties = reflect::Offset(cls, "Properties");
			a_kind.name = reflect::Offset(props, "Name");
			a_kind.icon = reflect::Offset(props, "Icon");
			a_kind.form = a_hasForm ? reflect::Offset(props, "form") : -1;
			if (a_hasForm) {
				a_kind.inContainer = reflect::Offset(props, "bIsInContainerMenu");
				a_kind.playerItem = reflect::Offset(props, "bIsInventoryItem");
			}
			if (a_kind.properties < 0 || a_kind.name < 0 || a_kind.icon < 0 || (a_hasForm && a_kind.form < 0)) {
				logger::error("rows: the {} row's layout is not as read (Properties {}, Name {}, Icon {}, form {}) - Favourite is off there",
					a_what, a_kind.properties, a_kind.name, a_kind.icon, a_kind.form);
				a_kind.rowClass = cls;   // do not ask again
				a_kind.properties = -1;
				return;
			}
			if (pe::Watch(cls, a_handler)) {
				a_kind.rowClass = cls;
				logger::info("rows: watching the {} rows (Properties at 0x{:X}: Name 0x{:X}, Icon 0x{:X}{})", a_what, a_kind.properties, a_kind.name,
					a_kind.icon, a_hasForm ? std::format(", form 0x{:X}", a_kind.form) : std::string());
			}
		}

		RE::TESSpellList* PlayerSpells()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* base = player ? player->data.objectReference : nullptr;
			if (!base || !base->Is(RE::FormType::NPC)) {
				return nullptr;
			}
			return static_cast<RE::TESSpellList*>(static_cast<RE::TESNPC*>(base));
		}
	}

	void Tick()
	{
		reflect::SelfCheck();
		TryWatch(g_inventory, kInventoryRowPath, kInventoryPropsPath, &OnInventoryRow, true, "inventory");
		TryWatch(g_magic, kMagicRowPath, kMagicPropsPath, &OnMagicRow, false, "magic menu");
	}

	std::uint32_t HighlightedItem()
	{
		Harvest();
		const auto id = FormOfInventoryRow(g_inventory.highlighted);
		logger::info("rows: highlighted inventory row: {} ({})", NameOfRow(g_inventory, g_inventory.highlighted), id ? std::format("0x{:08X}", id) : "no item");
		return id;
	}

	std::uint32_t HighlightedPlayerItemInContainer()
	{
		UE::UObject* row = g_inventory.highlighted;
		void* props = PropsOf(g_inventory, row);
		const bool* inContainer = reflect::At<bool>(props, g_inventory.inContainer);
		const bool* playerItem = reflect::At<bool>(props, g_inventory.playerItem);
		if (!inContainer || !playerItem || !*inContainer || !*playerItem) {
			return 0;
		}
		return FormOfInventoryRow(row);
	}

	std::uint32_t HighlightedSpell()
	{
		Harvest();
		const std::string name = NameOfRow(g_magic, g_magic.highlighted);
		const std::string key = KeyOfRow(g_magic, g_magic.highlighted);
		const auto id = SpellByName(key);
		logger::info("rows: highlighted magic row: \"{}\" (key {}; {})", name, key.empty() ? "none" : key,
			id ? std::format("spell 0x{:08X}", id) : "not one of the player's spells");
		return id;
	}

	UE::UObject* ItemIcon(std::uint32_t a_formID)
	{
		Harvest();
		const auto it = g_itemIcons.find(a_formID);
		return it == g_itemIcons.end() ? nullptr : it->second;
	}

	UE::UObject* SpellIcon(std::uint32_t a_formID)
	{
		Harvest();
		const auto it = g_spellIcons.find(a_formID);
		return it == g_spellIcons.end() ? nullptr : it->second;
	}

	std::uint32_t SpellByName(const std::string& a_name)
	{
		if (a_name.empty()) {
			return 0;
		}
		if (const auto it = g_spellsByName.find(a_name); it != g_spellsByName.end()) {
			return it->second;
		}
		auto* list = PlayerSpells();
		if (!list) {
			return 0;
		}
		for (RE::SpellItem* spell : list->spells) {
			const char* n = spell ? RE::TESFullName::GetFullName(spell) : nullptr;
			if (n && a_name == n) {
				g_spellsByName[a_name] = spell->GetFormID();
				return spell->GetFormID();
			}
		}
		return 0;
	}

	std::string SpellName(std::uint32_t a_formID)
	{
		auto* form = RE::TESForm::LookupByID(a_formID);
		const char* n = form ? RE::TESFullName::GetFullName(form) : nullptr;
		return n && *n ? std::string(n) : std::format("0x{:08X}", a_formID);
	}

	std::string Status()
	{
		return std::format("reflection {}; inventory rows {}; magic rows {}; icons known: {} items, {} spells",
			reflect::Ok() ? "proven" : "NOT proven", g_inventory.rowClass ? "watched" : "not loaded yet", g_magic.rowClass ? "watched" : "not loaded yet",
			g_itemIcons.size(), g_spellIcons.size());
	}
}
