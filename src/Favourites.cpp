#include "Favourites.h"

#include "Inventory.h"
#include "Menus.h"
#include "PEHook.h"
#include "Reflect.h"
#include "Rows.h"
#include "Settings.h"
#include "Ui.h"
#include "Wheels.h"

#include <unordered_map>

namespace favourites
{
	namespace
	{
		constexpr const wchar_t* kStarOutline = L"/Game/Art/UI/Common/T_UI_star_default_D.T_UI_star_default_D";   // the game's empty star
		constexpr const wchar_t* kButtonClass = L"/Game/UI/Modern/Prefabs/Buttons/WBP_ModernPrefab_InvisibleButton.WBP_ModernPrefab_InvisibleButton_C";
		constexpr double         kStarW = 38.0, kStarH = 19.0;   // the 64 x 32 star at the row's text height

		struct Column
		{
			std::int32_t    rowSlot = -1;   // the row's object-array slot when the column was added
			reflect::Handle overlay, fill, outline, button;
			std::string     key;            // what the row's name read when its item was last looked up
			std::uint32_t   form = 0;
			int             shown = -1;     // the fill as drawn: 1 favourite, 0 not, -1 not drawn yet
			bool            magic = false;  // a magic menu row (its star is the Magic wheel's)
		};
		std::unordered_map<UE::UObject*, Column> g_columns;   // by row; resolved through its slot before use

		reflect::Handle g_fillTexture, g_outlineTexture;
		bool            g_fillFailed = false;
		std::uint32_t   g_drawnGeneration = 0;
		ULONGLONG       g_next = 0;
		int             g_built = 0, g_clicks = 0;
		bool            g_watching = false;
		std::string     g_status = "the inventory has not been open yet";
		menus::Menu     g_lastMenu = menus::Menu::kNone;

		// ours, from the plugin folder: a texture the engine imports from the PNG (KismetRenderingLibrary)
		UE::UObject* FillTexture()
		{
			if (auto* t = reflect::Get(g_fillTexture)) return t;
			if (g_fillFailed) return nullptr;
			const auto path = settings::PluginFolder() / L"ImprovedWheelMenu" / L"FavouriteStarFill.png";
			static auto* lib = ui::Class(L"/Script/Engine.KismetRenderingLibrary");
			ui::Call c(lib ? lib->GetDefaultObject(false) : nullptr, L"ImportFileAsTexture2D");
			void* file = c.At("Filename");
			UE::UObject* tex = nullptr;
			if (c && file && std::filesystem::exists(path)) {
				c.Set("WorldContextObject", ui::PlayerController());
				auto* s = new (file) UE::FString(path.wstring().c_str());
				tex = c.RunGuarded() ? c.Get<UE::UObject*>("ReturnValue") : nullptr;
				s->~FString();
			}
			if (!tex) {
				g_fillFailed = true;
				logger::warn("favourites: the star's fill could not be loaded ({}{}) - a favourite shows the game's filled star instead",
					path.string(), std::filesystem::exists(path) ? "" : ", missing");
				return nullptr;
			}
			g_fillTexture = reflect::Hold(tex);
			logger::info("favourites: the star's fill is loaded from {}", path.filename().string());
			return tex;
		}

		UE::UObject* OutlineTexture()
		{
			if (auto* t = reflect::Get(g_outlineTexture)) return t;
			auto* t = ui::Load(kStarOutline);
			if (t) g_outlineTexture = reflect::Hold(t);
			return t;
		}

		UE::UObject* Prop(UE::UObject* a_o, std::string_view a_name)
		{
			const auto off = a_o && reflect::Ok() ? reflect::Offset(reinterpret_cast<UE::UStruct*>(a_o->GetClass()), a_name) : -1;
			return off >= 0 ? *reinterpret_cast<UE::UObject* const*>(reinterpret_cast<const std::uint8_t*>(a_o) + off) : nullptr;
		}

		UE::UObject* Add(UE::UObject* a_panel, const wchar_t* a_fn, UE::UObject* a_child)
		{
			ui::Call c(a_panel, a_fn);
			if (!c) return nullptr;
			c.Set("Content", a_child);
			return c.Run() ? c.Get<UE::UObject*>("ReturnValue") : nullptr;
		}

		void Align(UE::UObject* a_slot, std::uint8_t a_h, std::uint8_t a_v)
		{
			ui::CallFirst(a_slot, L"SetHorizontalAlignment", &a_h, sizeof(a_h));
			ui::CallFirst(a_slot, L"SetVerticalAlignment", &a_v, sizeof(a_v));
		}

		UE::UObject* StarImage(UE::UObject* a_tree, UE::UObject* a_overlay, const std::wstring& a_name, UE::UObject* a_texture)
		{
			static auto* imageClass = ui::Class(L"/Script/UMG.Image");
			auto* img = imageClass ? UE::NewObject<UE::UObject>(a_tree, imageClass, UE::FName(a_name.c_str())) : nullptr;
			auto* slot = img ? Add(a_overlay, L"AddChildToOverlay", img) : nullptr;
			if (!slot) return nullptr;
			Align(slot, 2, 2);   // centred
			if (a_texture) {
				ui::Call b(img, L"SetBrushFromTexture");
				b.Set("Texture", a_texture);
				b.Set("bMatchSize", false);
				b.Run();
			}
			ui::Vec2(img, L"SetDesiredSizeOverride", kStarW, kStarH);
			ui::Visible(img, true);
			return img;
		}

		void OnButton(UE::UObject* a_obj, UE::UFunction* a_fn, void*);

		// the column, added once to a row
		bool Build(UE::UObject* a_row, Column& a_col, const char* a_horizontal)
		{
			static auto* overlayClass = ui::Class(L"/Script/UMG.Overlay");
			auto* horizontal = Prop(a_row, a_horizontal);
			if (!horizontal) horizontal = ui::FindInTree(a_row, a_horizontal);
			auto* tree = Prop(a_row, "WidgetTree");
			if (!overlayClass || !horizontal || !tree) {
				static bool logged = false;
				if (!logged) logger::warn("favourites: a row has no {} - no column", !horizontal ? a_horizontal : "widget tree");
				if (!logged && !horizontal) logger::warn("favourites: the row's tree root is {}", tree ? pe::Utf8(tree->GetFName().ToString()) : std::string("none"));
				logged = true;
				return false;
			}
			const std::wstring n = std::to_wstring(++g_built);
			auto* overlay = UE::NewObject<UE::UObject>(tree, overlayClass, UE::FName((L"IwmFavourite" + n).c_str()));
			auto* slot = overlay ? Add(horizontal, L"AddChildToHorizontalBox", overlay) : nullptr;
			if (!slot) return false;
			Align(slot, 2, 2);
			const float pad[4] = { 10.0f, 0.0f, 6.0f, 0.0f };   // left, top, right, bottom
			ui::CallFirst(slot, L"SetPadding", pad, sizeof(pad));

			auto* fillTex = FillTexture();
			auto* fill = StarImage(tree, overlay, L"IwmFavouriteFill" + n, fillTex ? fillTex : ui::Load(L"/Game/Art/UI/Common/T_UI_star_D.T_UI_star_D"));
			auto* outline = StarImage(tree, overlay, L"IwmFavouriteStar" + n, OutlineTexture());
			if (fill) ui::Visible(fill, false);

			// the click surface: the game's own invisible button, as Simple Loadout System's boxes use it; never a stop for
			// the controller's navigation (IsFocusable off before its Slate widget is built)
			auto* button = ui::CreateWidget(kButtonClass);
			if (button) {
				static const auto focusable = reflect::Offset(reinterpret_cast<UE::UStruct*>(button->GetClass()), "IsFocusable");
				if (focusable >= 0) *(reinterpret_cast<std::uint8_t*>(button) + focusable) = 0;
				if (auto* bs = Add(overlay, L"AddChildToOverlay", button)) {
					Align(bs, 0, 0);   // fills the star
				}
				if (!g_watching) {
					g_watching = pe::Watch(button->GetClass(), &OnButton);
					logger::info("favourites: {} the star buttons' clicks", g_watching ? "watching" : "could NOT watch");
				}
			}
			a_col.overlay = reflect::Hold(overlay);
			a_col.fill = fill ? reflect::Hold(fill) : reflect::Handle{};
			a_col.outline = outline ? reflect::Hold(outline) : reflect::Handle{};
			a_col.button = button ? reflect::Hold(button) : reflect::Handle{};
			a_col.rowSlot = a_row->internalIndex;
			a_col.shown = -1;
			a_col.key.clear();
			static bool firstLogged = false;
			if (!firstLogged) {
				firstLogged = true;
				logger::info("favourites: the first row's column is built (fill {}, outline {}, button {})", fill ? "yes" : "NO", outline ? "yes" : "NO",
					button ? "yes" : "NO");
			}
			return true;
		}

		// a star's button clicked: its row's item on or off the wheel, as Y
		void OnButton(UE::UObject* a_obj, UE::UFunction* a_fn, void*)
		{
			if (!a_obj || !a_fn || pe::FunctionName(a_fn) != "BP_OnClicked") return;
			for (auto& [row, col] : g_columns) {
				if (reflect::Get(col.button) != a_obj) continue;
				auto* live = reflect::Get(row, col.rowSlot);
				const auto form = !live ? 0 : col.magic ? rows::MagicRowSpell(live) : rows::InventoryRowForm(live);
				if (!form) {
					logger::info("favourites: a star was clicked on a row with no {}", col.magic ? "spell of the player's" : "item");
					return;
				}
				++g_clicks;
				if (col.magic) {
					logger::info("favourites: star clicked on the spell {}", rows::SpellName(form));
					wheels::ToggleSpell(form);
				} else {
					logger::info("favourites: star clicked on {}", inventory::NameOf(form));
					wheels::ToggleItem(form);
				}
				g_next = 0;   // redrawn at once
				return;
			}
		}
	}

	void Tick()
	{
		// the inventory's rows (items: the Equipment and Ammo wheels) and the magic menu's (spells: the Magic wheel) - the
		// owner, 2026-09-30: "we need to add a star for favorites on the column in the magic menu"
		const auto menu = menus::Active();
		const bool magic = menu == menus::Menu::kMagic;
		if (menu != menus::Menu::kInventory && !magic) {
			g_lastMenu = menu;
			return;
		}
		const ULONGLONG now = GetTickCount64();
		if (now < g_next) return;
		g_next = now + 100;
		const bool menuChanged = menu != g_lastMenu;
		g_lastMenu = menu;
		const bool rowsChanged = magic ? rows::TakeMagicRowsChanged() : rows::TakeInventoryRowsChanged();
		const auto generation = wheels::Generation();
		if (!menuChanged && !rowsChanged && generation == g_drawnGeneration) return;   // nothing moved: nothing to do
		g_drawnGeneration = generation;

		const auto live = magic ? rows::LiveMagicRows() : rows::LiveInventoryRows();
		const auto favs = magic ? wheels::MagicFavourites() : wheels::Favourites();
		int drawn = 0, built = 0;
		for (auto* row : live) {
			Column& col = g_columns[row];
			if (col.rowSlot != row->internalIndex || !reflect::Get(col.overlay)) {
				col = {};
				col.magic = magic;
				if (!Build(row, col, magic ? "Magic_entry_horizontal" : "inv_entry_horizontal")) continue;
				++built;
			}
			const std::string key = magic ? rows::MagicRowKey(row) : rows::InventoryRowKey(row);
			if (key != col.key) {   // the row shows another item or spell now
				col.key = key;
				col.form = magic ? rows::MagicRowSpell(row) : rows::InventoryRowForm(row);
				col.shown = -1;
			}
			const int fav = col.form && favs.contains(col.form) ? 1 : 0;
			if (fav != col.shown) {
				ui::Visible(reflect::Get(col.fill), fav == 1);
				col.shown = fav;
				++drawn;
			}
		}
		// rows gone for good leave the map
		for (auto it = g_columns.begin(); it != g_columns.end();) {
			it = reflect::Get(it->first, it->second.rowSlot) ? std::next(it) : g_columns.erase(it);
		}
		if (built || drawn) {
			g_status = std::format("{} rows with a column ({} built now in the {}), {} stars redrawn, {} clicks so far", g_columns.size(), built,
				magic ? "magic menu" : "inventory", drawn, g_clicks);
			logger::debug("favourites: {}", g_status);
		}
	}

	std::string Status() { return g_status; }
}
