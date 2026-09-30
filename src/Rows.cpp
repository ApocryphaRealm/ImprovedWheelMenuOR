#include "Rows.h"

#include "PEHook.h"
#include "Inventory.h"
#include "Reflect.h"
#include "Ui.h"

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
			reflect::Handle highlighted;   // kept across frames: resolved through its slot before every use
			UE::UFunction* fnSelection = nullptr;   // this row class's BP_OnItemSelectionChanged
			std::unordered_map<UE::UObject*, std::int32_t> seen;   // row -> its object-array slot
		};
		Kind g_inventory, g_magic;

		std::unordered_map<std::uint32_t, reflect::Handle> g_itemIcons, g_spellIcons;   // resolved through the slot when asked
		std::unordered_map<std::string, std::uint32_t> g_spellsByName;
		bool g_dirty = true;   // a row changed since the caches were last filled

		// a_row must be live NOW: resolved from its Handle (or its seen slot) this frame, never a pointer kept from before
		void* PropsOf(Kind& a_kind, UE::UObject* a_row)
		{
			return a_row && a_kind.properties >= 0 ? reflect::At<void>(a_row, a_kind.properties) : nullptr;
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
				auto* row = reflect::Get(it->first, it->second);
				if (!row) {
					it = g_inventory.seen.erase(it);
					continue;
				}
				if (const auto id = FormOfInventoryRow(row)) {
					if (auto* icon = IconOfRow(g_inventory, row); icon && reflect::IsLive(icon)) {
						g_itemIcons[id] = reflect::Hold(icon);
					}
				}
				++it;
			}
			for (auto it = g_magic.seen.begin(); it != g_magic.seen.end();) {
				auto* row = reflect::Get(it->first, it->second);
				if (!row) {
					it = g_magic.seen.erase(it);
					continue;
				}
				if (const auto id = SpellByName(KeyOfRow(g_magic, row))) {
					if (auto* icon = IconOfRow(g_magic, row); icon && reflect::IsLive(icon)) {
						g_spellIcons[id] = reflect::Hold(icon);
					}
				}
				++it;
			}
		}

		void OnRow(Kind& a_kind, UE::UObject* a_obj, UE::UFunction* a_fn, void* a_params)
		{
			a_kind.seen.insert_or_assign(a_obj, a_obj->internalIndex);   // live: the engine is calling it now
			g_dirty = true;
			if (!a_kind.fnSelection && pe::FunctionName(a_fn) == "BP_OnItemSelectionChanged") {
				a_kind.fnSelection = a_fn;
			}
			if (a_fn == a_kind.fnSelection && a_params && *static_cast<const bool*>(a_params)) {
				a_kind.highlighted = reflect::Hold(a_obj);
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
		auto*      row = reflect::Get(g_inventory.highlighted);   // gone after a drop or a close: no item
		const auto id = FormOfInventoryRow(row);
		logger::info("rows: highlighted inventory row: {} ({})", row ? NameOfRow(g_inventory, row) : std::string("(gone)"),
			id ? std::format("0x{:08X}", id) : "no item");
		return id;
	}

	std::uint32_t HighlightedPlayerItemInContainer()
	{
		UE::UObject* row = reflect::Get(g_inventory.highlighted);
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
		auto*             row = reflect::Get(g_magic.highlighted);
		const std::string name = NameOfRow(g_magic, row);
		const std::string key = KeyOfRow(g_magic, row);
		const auto id = SpellByName(key);
		logger::info("rows: highlighted magic row: \"{}\" (key {}; {})", name, key.empty() ? "none" : key,
			id ? std::format("spell 0x{:08X}", id) : "not one of the player's spells");
		return id;
	}

	namespace
	{
		// The form's own icon path (TESIcon), as the plugin data gives it: "Weapons\IronArrow.dds"
		const RE::TESIcon* IconOf(RE::TESForm* a_form)
		{
			if (!a_form) return nullptr;
			switch (a_form->GetFormType()) {
			case RE::FormType::Ammo: return a_form->As<RE::TESAmmo>();
			case RE::FormType::Weapon: return a_form->As<RE::TESObjectWEAP>();
			case RE::FormType::Book: return a_form->As<RE::TESObjectBOOK>();
			case RE::FormType::Misc: return a_form->As<RE::TESObjectMISC>();
			case RE::FormType::Apparatus: return a_form->As<RE::TESObjectAPPA>();
			case RE::FormType::Ingredient: return a_form->As<RE::IngredientItem>();
			case RE::FormType::AlchemyItem: return a_form->As<RE::AlchemyItem>();
			case RE::FormType::Light: return a_form->As<RE::TESObjectLIGH>();
			default: return nullptr;
			}
		}

		// The remaster's texture for that path (the primary session's reads, 2026-09-30: Arrow1Iron's icon is
		// "Weapons\IronArrow.dds", and the paks hold /Game/Art/UI/Icons/Dynamic_Icons/menus/icons/weapons/T_ironarrow -
		// the folder in lower case, "T_" and the lower-case stem; the HUD's own textures follow the same rule)
		std::string IconAssetPath(std::string_view a_icon)
		{
			std::string p(a_icon);
			for (auto& c : p) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (const auto dot = p.rfind('.'); dot != std::string::npos) p.resize(dot);
			const auto slash = p.rfind('/');
			const std::string dir = slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
			const std::string stem = slash == std::string::npos ? p : p.substr(slash + 1);
			if (stem.empty()) return {};
			return "/Game/Art/UI/Icons/Dynamic_Icons/menus/icons/" + dir + "T_" + stem + ".T_" + stem;
		}

		std::unordered_set<std::uint32_t> g_iconFailed;   // forms whose icon path loaded nothing: not tried again
	}

	// the icon a row showed for it this session; else the form's own icon, loaded from the paks (works straight after a
	// load - the owner, 2026-09-30: no arrows on the ammo wheel until the inventory had shown them)
	UE::UObject* ItemIcon(std::uint32_t a_formID)
	{
		Harvest();
		if (const auto it = g_itemIcons.find(a_formID); it != g_itemIcons.end()) {
			if (auto* icon = reflect::Get(it->second)) return icon;
		}
		if (!a_formID || g_iconFailed.contains(a_formID)) return nullptr;
		const auto* tex = IconOf(RE::TESForm::LookupByID(a_formID));
		const char* icon = tex ? tex->textureName.c_str() : nullptr;
		const auto  path = icon && *icon ? IconAssetPath(icon) : std::string();
		const std::wstring wpath(path.begin(), path.end());   // the path is ASCII
		auto*       loaded = path.empty() ? nullptr : ui::Load(wpath.c_str());
		if (!loaded) {
			g_iconFailed.insert(a_formID);
			logger::info("rows: no icon for 0x{:08X} from its form ({}{})", a_formID, icon && *icon ? icon : "no icon path",
				path.empty() ? std::string() : " -> " + path + " did not load");
			return nullptr;
		}
		g_itemIcons[a_formID] = reflect::Hold(loaded);
		logger::info("rows: icon for 0x{:08X} from its form: {} -> {}", a_formID, icon, path);
		return loaded;
	}

	UE::UObject* SpellIcon(std::uint32_t a_formID)
	{
		Harvest();
		const auto it = g_spellIcons.find(a_formID);
		return it == g_spellIcons.end() ? nullptr : reflect::Get(it->second);
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
