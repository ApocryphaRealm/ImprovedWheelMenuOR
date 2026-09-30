#include "Ammo.h"

#include "Inventory.h"
#include "Reflect.h"
#include "Rows.h"
#include "Settings.h"
#include "TesThread.h"
#include "Ui.h"
#include "Wheels.h"

#include <numbers>

namespace ammo
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr int    kSlots = 8;
		constexpr double kStickOn = 0.45;     // the right stick points once it is this far out
		constexpr double kStickRest = 0.25;   // and is at rest inside this
		constexpr int    kBowType = 5;        // TESObjectWEAP data.type: 0-1 blade, 2-3 blunt, 4 staff, 5 bow
		constexpr WORD   kDpad = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
		constexpr const wchar_t* kFullNames = L"/Game/Localization/StringTables/ST_FullNames.ST_FullNames";

		// ---- the widget (game thread; kept across frames as slot-checked handles) ----
		reflect::Handle g_root, g_label;
		std::array<reflect::Handle, kSlots> g_icon;
		std::array<std::uint32_t, kSlots>   g_shownIds{};
		int       g_builds = 0;
		ULONGLONG g_lastBuild = 0;
		std::string g_shownText;

		// ---- state (game thread) ----
		bool              g_open = false;
		int               g_pointed = -1;
		bool              g_pointedByStick = false;
		bool              g_resting = true;
		Clock::time_point g_restSince{};
		std::array<std::uint32_t, kSlots> g_ids{};   // what the wheel holds while it is open
		std::uint32_t     g_worn = 0;                // the arrows worn when it opened
		int               g_drawnPointed = -2;
		WORD              g_swallow = 0;             // buttons the wheel used: the game never sees them, until let go
		std::string       g_last = "never opened";
		std::uint32_t     g_opens = 0, g_equips = 0;

		RE::BSSimpleList<RE::ItemChange*>* Items()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* changes = player ? player->extra.GetExtraData<RE::ExtraContainerChanges>() : nullptr;
			return changes && changes->changes ? changes->changes->list : nullptr;
		}

		bool Worn(RE::ExtraDataList* a_list)
		{
			return a_list && (a_list->GetExtraData(RE::EXTRA_DATA_TYPE::Worn) || a_list->GetExtraData(RE::EXTRA_DATA_TYPE::WornLeft));
		}

		// a bow worn - and which arrows are (0 = none)
		bool BowHeld(std::uint32_t& a_wornAmmo)
		{
			a_wornAmmo = 0;
			bool bow = false;
			auto* items = Items();
			if (!items) {
				return false;
			}
			for (RE::ItemChange* item : *items) {
				if (!item || !item->object || !item->extraData || item->count <= 0) {
					continue;
				}
				bool worn = false;
				for (RE::ExtraDataList* xl : *item->extraData) {
					worn = worn || Worn(xl);
				}
				if (!worn) {
					continue;
				}
				const auto type = item->object->GetFormType();
				if (type == RE::FormType::Weapon) {
					auto* weap = item->object->As<RE::TESObjectWEAP>();
					bow = bow || (weap && weap->data.type == kBowType);
				} else if (type == RE::FormType::Ammo) {
					a_wornAmmo = item->object->GetFormID();
				}
			}
			return bow;
		}

		double Radius() { return 230.0 * settings::Get().ammoScalePercent / 100.0; }

		// where arrow i sits: on the left half of a circle centred on the middle of the right-hand edge, the top one first
		std::array<double, 2> Place(int a_i)
		{
			const double R = Radius(), r = R * 0.68;
			const double th = (90.0 + (a_i + 0.5) * 180.0 / kSlots) * std::numbers::pi / 180.0;
			return { R + r * std::cos(th), R - r * std::sin(th) };
		}

		void White(UE::UObject* a_label)
		{
			struct SlateColor
			{
				float        rgba[4];
				std::uint8_t rule;   // ESlateColorStylingMode: 0 = the colour given
				std::uint8_t pad[7];
			} white{ { 1.0f, 1.0f, 1.0f, 1.0f }, 0, {} };
			ui::CallFirst(a_label, L"SetColor", &white, sizeof(white));   // the prefab's own (Tween Menu, BTPS)
			ui::CallFirst(a_label, L"SetColorAndOpacity", &white, sizeof(white));
			const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.85f };
			ui::CallFirst(a_label, L"SetShadowColorAndOpacity", shadow, sizeof(shadow));
			ui::Vec2(a_label, L"SetShadowOffset", 1.5, 1.5);
		}

		bool Build()
		{
			const ULONGLONG now = GetTickCount64();
			if (g_lastBuild && now - g_lastBuild < 2000) {
				return false;
			}
			g_lastBuild = now;
			auto* root = ui::CreateWidget(L"/Script/UMG.UserWidget");
			const std::wstring n = std::to_wstring(++g_builds);
			auto* canvas = root ? ui::RootCanvas(root, (L"IwmAmmoCanvas" + n).c_str()) : nullptr;
			if (!canvas) {
				logger::info("ammo: the wheel cannot be built yet ({})", !ui::PlayerController() ? "no player controller" :
					!root ? "the user widget could not be created" : "its canvas could not be made");
				return false;
			}
			const double R = Radius();
			UE::UObject* panelSlot = nullptr;
			auto* panel = ui::AddToCanvas(canvas, L"/Script/UMG.CanvasPanel", (L"IwmAmmoPanel" + n).c_str(), &panelSlot);
			if (!panel || !panelSlot) {
				logger::warn("ammo: the wheel's panel could not be made");
				return false;
			}
			ui::Anchors(panelSlot, 1.0, 0.5, 1.0, 0.5);   // locked to the middle of the right-hand edge
			ui::Vec2(panelSlot, L"SetAlignment", 1.0, 0.5);
			ui::Vec2(panelSlot, L"SetPosition", 0.0, 0.0);
			ui::Vec2(panelSlot, L"SetSize", R, 2.0 * R);
			const std::uint8_t clip = 1;   // EWidgetClipping::ClipToBounds: the round back shows its left half - the semi-circle
			ui::CallFirst(panel, L"SetClipping", &clip, sizeof(clip));

			UE::UObject* backSlot = nullptr;
			if (auto* back = ui::AddToCanvas(panel, L"/Script/UMG.Image", (L"IwmAmmoBack" + n).c_str(), &backSlot)) {
				ui::Anchors(backSlot, 0, 0, 0, 0);
				ui::Vec2(backSlot, L"SetPosition", 0.0, 0.0);
				ui::Vec2(backSlot, L"SetSize", 2.0 * R, 2.0 * R);
				const float fill[4] = { 0.05f, 0.04f, 0.03f, 0.72f };
				const float edge[4] = { 0.90f, 0.87f, 0.82f, 0.85f };
				if (!ui::RoundedBox(back, true, fill, edge, 3.0f)) {
					logger::warn("ammo: the wheel's round back could not be drawn (the brush fields were not found)");
				}
			}
			for (int i = 0; i < kSlots; ++i) {
				g_icon[i] = {};
				UE::UObject* s = nullptr;
				auto* img = ui::AddToCanvas(panel, L"/Script/UMG.Image", (L"IwmAmmoIcon" + n + L"_" + std::to_wstring(i)).c_str(), &s);
				if (!img) {
					continue;
				}
				ui::Anchors(s, 0, 0, 0, 0);
				ui::Vec2(s, L"SetAlignment", 0.5, 0.5);
				const auto p = Place(i);
				ui::Vec2(s, L"SetPosition", p[0], p[1]);
				ui::Vec2(s, L"SetSize", R * 0.24, R * 0.24);
				ui::Vec2(img, L"SetRenderTransformPivot", 0.5, 0.5);
				ui::Visible(img, false);
				g_icon[i] = reflect::Hold(img);
			}
			// the pointed arrows' name, left of the wheel (outside the clipped panel)
			UE::UObject* labelSlot = nullptr;
			auto* label = ui::AddToCanvas(canvas, L"/Game/UI/Modern/Prefabs/WBP_AltarTextBlock.WBP_AltarTextBlock_C", (L"IwmAmmoLabel" + n).c_str(),
				&labelSlot);
			if (label) {
				ui::Anchors(labelSlot, 1.0, 0.5, 1.0, 0.5);
				ui::Vec2(labelSlot, L"SetAlignment", 1.0, 0.5);
				ui::Vec2(labelSlot, L"SetPosition", -R - 12.0, 0.0);
				const bool yes = true;
				ui::CallFirst(labelSlot, L"SetAutoSize", &yes, sizeof(yes));
			} else {
				logger::info("ammo: the game's text prefab is not loaded - the wheel shows no name");
			}
			ui::Call vp(root, L"AddToViewport");
			vp.Set<std::int32_t>("ZOrder", 20);
			vp.Run();
			if (label) {
				White(label);   // after the Slate widget exists
			}
			g_root = reflect::Hold(root);
			g_label = label ? reflect::Hold(label) : reflect::Handle{};
			g_shownIds = {};
			g_shownText.clear();
			g_drawnPointed = -2;
			ui::Visible(root, false);
			logger::info("ammo: the wheel is built (build {}; a half circle {:.0f} tall on the middle of the right-hand edge)", g_builds, 2.0 * R);
			return true;
		}

		bool Built() { return reflect::Get(g_root) != nullptr; }

		// the name of what is pointed: a string-table key (LOC_FN_...) through the game's own table, as the UI shows it
		void SetLabel(const std::string& a_text)
		{
			auto* label = reflect::Get(g_label);
			if (!label || a_text == g_shownText) {
				return;
			}
			auto* fn = label->FindFunction(UE::FName(L"SetText", UE::EFindName::Find));
			const auto params = fn ? reflect::Fields(reinterpret_cast<UE::UStruct*>(fn)) : std::vector<std::pair<std::string, std::int32_t>>{};
			if (params.empty()) {
				return;
			}
			const std::wstring w(a_text.begin(), a_text.end());
			std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::max(reinterpret_cast<UE::UStruct*>(fn)->propertiesSize, 0)) + 16, 0);
			UE::FText* text = nullptr;
			if (a_text.starts_with("LOC_")) {
				static auto* lib = ui::Class(L"/Script/Engine.KismetTextLibrary");
				ui::Call t(lib ? lib->GetDefaultObject(false) : nullptr, L"TextFromStringTable");
				void* id = t.At("TableId");
				void* key = t.At("Key");
				void* ret = t.At("ReturnValue");
				if (id && key && ret) {
					new (id) UE::FName(kFullNames, UE::EFindName::Add);
					auto* k = new (key) UE::FString(w.c_str());
					t.Run();
					auto* got = static_cast<UE::FText*>(ret);
					text = new (buf.data() + params.front().second) UE::FText(*got);
					got->~FText();
					k->~FString();
				}
			}
			if (!text) {
				text = new (buf.data() + params.front().second) UE::FText(UE::FText::AsCultureInvariant(UE::FString(w.c_str())));
			}
			label->ProcessEvent(fn, buf.data());
			text->~FText();
			g_shownText = a_text;
			White(label);
		}

		void Draw()
		{
			for (int i = 0; i < kSlots; ++i) {
				auto* img = reflect::Get(g_icon[i]);
				if (!img || g_ids[i] == g_shownIds[i]) {
					continue;
				}
				auto* icon = g_ids[i] ? rows::ItemIcon(g_ids[i]) : nullptr;
				if (icon) {
					ui::Call b(img, L"SetBrushFromTexture");
					b.Set("Texture", icon);
					b.Set("bMatchSize", false);
					b.Run();
				} else if (g_ids[i]) {
					logger::info("ammo: no picture for {} yet - one shows once an inventory row has shown it this session", inventory::NameOf(g_ids[i]));
				}
				ui::Visible(img, icon != nullptr);
				g_shownIds[i] = g_ids[i];
				g_drawnPointed = -2;
			}
			if (g_pointed == g_drawnPointed) {
				return;
			}
			for (int i = 0; i < kSlots; ++i) {
				auto* img = reflect::Get(g_icon[i]);
				if (!img || !g_ids[i]) {
					continue;
				}
				const bool on = i == g_pointed;
				ui::Vec2(img, L"SetRenderScale", on ? 1.35 : 1.0, on ? 1.35 : 1.0);
				if (g_ids[i] == g_worn) {
					ui::Colour(img, 1.0f, 0.82f, 0.35f, on ? 1.0f : 0.85f);   // the arrows worn now: gold
				} else {
					ui::Colour(img, 1.0f, 1.0f, 1.0f, on ? 1.0f : 0.7f);
				}
			}
			SetLabel(g_pointed >= 0 && g_ids[g_pointed] ? inventory::NameOf(g_ids[g_pointed]) : std::string(" "));
			g_drawnPointed = g_pointed;
		}

		void Close(const char* a_why)
		{
			g_open = false;
			g_pointed = -1;
			ui::Visible(reflect::Get(g_root), false);
			logger::info("ammo: the wheel closed ({})", a_why);
		}

		void Equip(std::uint32_t a_id)
		{
			testhread::Post([a_id]() {
				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* items = Items();
				if (!player || !items) {
					return;
				}
				for (RE::ItemChange* item : *items) {
					if (item && item->object && item->object->GetFormID() == a_id && item->count > 0) {
						// arrows are worn as the whole stack; no lock (EquipObject's last argument is the console's NoUnequip)
						player->EquipObject(item->object, item->count, nullptr, false, false);
						logger::info("ammo: {} x{} equipped (TES thread)", inventory::NameOf(a_id), item->count);
						return;
					}
				}
				logger::info("ammo: {} is not carried any more - nothing equipped", inventory::NameOf(a_id));
			});
		}

		// the next filled slot from a_from (-1 = none yet) in a_dir
		int Step(int a_from, int a_dir)
		{
			int i = a_from < 0 ? (a_dir > 0 ? -1 : kSlots) : a_from;
			for (int k = 0; k < kSlots; ++k) {
				i = (i + a_dir + kSlots) % kSlots;
				if (g_ids[i]) {
					return i;
				}
			}
			return -1;
		}

		// the filled slot nearest where the right stick points (y up), or -1
		int Aim(double a_x, double a_y)
		{
			double th = std::atan2(a_y, a_x) * 180.0 / std::numbers::pi;
			if (th < 0) {
				th += 360.0;
			}
			int i = 0;
			if (th >= 90.0 && th <= 270.0) {
				i = std::clamp(static_cast<int>((th - 90.0) / (180.0 / kSlots)), 0, kSlots - 1);
			} else {
				i = a_y >= 0 ? 0 : kSlots - 1;   // pointed right, off the wheel: the nearer end
			}
			for (int d = 0; d < kSlots; ++d) {
				if (i - d >= 0 && g_ids[i - d]) {
					return i - d;
				}
				if (i + d < kSlots && g_ids[i + d]) {
					return i + d;
				}
			}
			return -1;
		}
	}

	bool Rewrite(XINPUT_GAMEPAD& a_pad, WORD a_raw, WORD a_pressed, WORD& a_out, bool a_gameplay, bool a_radialOpen)
	{
		g_swallow &= a_raw;
		a_out &= ~g_swallow;
		const auto& s = settings::Get();
		const WORD button = static_cast<WORD>(s.ammoButton);

		if (!g_open) {
			if (!s.ammoWheel || !button || !a_gameplay || a_radialOpen || !(a_pressed & button)) {
				return false;
			}
			std::uint32_t worn = 0;
			if (!BowHeld(worn)) {
				return false;   // without a bow the button is the game's
			}
			if (!Built() && !Build()) {
				return false;
			}
			g_ids = wheels::AmmoSlots();
			g_worn = worn;
			int filled = 0;
			for (const auto id : g_ids) {
				filled += id ? 1 : 0;
			}
			g_open = true;
			++g_opens;
			g_pointed = -1;
			for (int i = 0; i < kSlots && g_pointed < 0; ++i) {
				g_pointed = g_ids[i] && g_ids[i] == worn ? i : -1;   // starts on the arrows worn now
			}
			g_pointedByStick = false;
			g_resting = true;
			g_drawnPointed = -2;
			Draw();
			ui::Visible(reflect::Get(g_root), true);
			g_swallow |= button;
			a_out &= ~button;
			g_last = std::format("opened with {} arrow kind(s)", filled);
			logger::info("ammo: the wheel opened - {} arrow kind(s) on it{}", filled, filled ? "" : " (Y on arrows in the inventory puts them here)");
			return true;
		}

		if (!a_gameplay || a_radialOpen) {
			Close("a menu opened");
			return false;
		}
		// open: the D-pad, A, B and the right stick are the wheel's
		a_out &= ~(kDpad | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B);
		const double x = a_pad.sThumbRX / 32767.0, y = a_pad.sThumbRY / 32767.0;
		const double m = std::hypot(x, y);
		a_pad.sThumbRX = 0;
		a_pad.sThumbRY = 0;
		const auto now = Clock::now();
		if (m >= kStickOn) {
			if (const int aim = Aim(x, y); aim >= 0) {
				g_pointed = aim;
				g_pointedByStick = true;
			}
			g_resting = false;
		} else if (m < kStickRest) {
			if (!g_resting) {
				g_resting = true;
				g_restSince = now;
			}
			// the centre rest snap: the stick back in the middle for a moment points at nothing
			if (s.centreRestSnap && g_pointedByStick && g_pointed >= 0 && now - g_restSince >= std::chrono::milliseconds(s.restSnapMs)) {
				g_pointed = -1;
				g_pointedByStick = false;
			}
		}
		if (a_pressed & XINPUT_GAMEPAD_DPAD_UP) {
			g_pointed = Step(g_pointed, -1);
			g_pointedByStick = false;
		}
		if (a_pressed & XINPUT_GAMEPAD_DPAD_DOWN) {
			g_pointed = Step(g_pointed, +1);
			g_pointedByStick = false;
		}
		if (a_pressed & XINPUT_GAMEPAD_B) {
			g_swallow |= XINPUT_GAMEPAD_B;
			g_last = "closed with B";
			Close("B - nothing equipped");
		} else if (a_pressed & (button | XINPUT_GAMEPAD_A)) {
			g_swallow |= a_pressed & (button | XINPUT_GAMEPAD_A);
			if (g_pointed >= 0 && g_ids[g_pointed]) {
				++g_equips;
				g_last = std::format("equipped {}", inventory::NameOf(g_ids[g_pointed]));
				Equip(g_ids[g_pointed]);
				Close("the pointed arrows go on");
			} else {
				g_last = "closed with nothing pointed";
				Close("nothing pointed");
			}
		} else {
			Draw();
		}
		return true;
	}

	bool Open() { return g_open; }

	nlohmann::json State()
	{
		const auto t = testhread::GetStatus();
		nlohmann::json ids = nlohmann::json::array();
		for (const auto id : wheels::AmmoSlots()) {
			ids.push_back(id ? inventory::NameOf(id) : "");
		}
		return { { "open", g_open }, { "pointed", g_pointed }, { "built", Built() }, { "builds", g_builds }, { "opens", g_opens },
			{ "equips", g_equips }, { "last", g_last }, { "slots", ids }, { "tes_thread", t.installed }, { "tes_thread_calls", t.calls },
			{ "tes_thread_queued", t.queued } };
	}
}
