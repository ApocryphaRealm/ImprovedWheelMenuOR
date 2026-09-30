#include "WheelName.h"

#include "Menus.h"
#include "Pad.h"
#include "QuickKeys.h"
#include "Reflect.h"
#include "Ui.h"
#include "Wheels.h"

namespace wheelname
{
	namespace
	{
		reflect::Handle g_root, g_label, g_slot;
		int         g_builds = 0;
		ULONGLONG   g_lastBuild = 0, g_nextLook = 0;
		bool        g_shown = false;
		std::string g_text;
		double      g_x = -1e9, g_y = -1e9;

		void White(UE::UObject* a_label)
		{
			struct SlateColor
			{
				float        rgba[4];
				std::uint8_t rule;
				std::uint8_t pad[7];
			} white{ { 1.0f, 1.0f, 1.0f, 1.0f }, 0, {} };
			ui::CallFirst(a_label, L"SetColor", &white, sizeof(white));
			ui::CallFirst(a_label, L"SetColorAndOpacity", &white, sizeof(white));
			const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.9f };
			ui::CallFirst(a_label, L"SetShadowColorAndOpacity", shadow, sizeof(shadow));
			ui::Vec2(a_label, L"SetShadowOffset", 1.5, 1.5);
		}

		bool Build()
		{
			const ULONGLONG now = GetTickCount64();
			if (g_lastBuild && now - g_lastBuild < 2000) return false;
			g_lastBuild = now;
			auto* root = ui::CreateWidget(L"/Script/UMG.UserWidget");
			const std::wstring n = std::to_wstring(++g_builds);
			auto* canvas = root ? ui::RootCanvas(root, (L"IwmNameCanvas" + n).c_str()) : nullptr;
			if (!canvas) return false;
			UE::UObject* slot = nullptr;
			auto* label = ui::AddToCanvas(canvas, L"/Game/UI/Modern/Prefabs/WBP_AltarTextBlock.WBP_AltarTextBlock_C", (L"IwmNameLabel" + n).c_str(), &slot);
			if (!label || !slot) {
				logger::info("wheel name: the game's text prefab is not loaded yet");
				return false;
			}
			ui::Anchors(slot, 0, 0, 0, 0);
			ui::Vec2(slot, L"SetAlignment", 0.5, 1.0);   // centred, its bottom on the point given
			const bool yes = true;
			ui::CallFirst(slot, L"SetAutoSize", &yes, sizeof(yes));
			ui::Call vp(root, L"AddToViewport");
			vp.Set<std::int32_t>("ZOrder", 60);   // above the menus' own layers
			vp.Run();
			White(label);
			ui::Visible(root, false);
			g_root = reflect::Hold(root);
			g_label = reflect::Hold(label);
			g_slot = reflect::Hold(slot);
			g_shown = false;
			g_text.clear();
			g_x = g_y = -1e9;
			logger::info("wheel name: the label is built (build {})", g_builds);
			return true;
		}

		void SetText(UE::UObject* a_label, const std::string& a_text)
		{
			auto* fn = a_label->FindFunction(UE::FName(L"SetText", UE::EFindName::Find));
			const auto params = fn ? reflect::Fields(reinterpret_cast<UE::UStruct*>(fn)) : std::vector<std::pair<std::string, std::int32_t>>{};
			if (params.empty()) return;
			std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::max(reinterpret_cast<UE::UStruct*>(fn)->propertiesSize, 0)) + 16, 0);
			const std::wstring w(a_text.begin(), a_text.end());
			auto* text = new (buf.data() + params.front().second) UE::FText(UE::FText::AsCultureInvariant(UE::FString(w.c_str())));
			a_label->ProcessEvent(fn, buf.data());
			text->~FText();
			White(a_label);
		}

		// where the wheel widget is on the viewport (viewport units): its top-left and its width
		bool Measure(UE::UObject* a_w, double& a_x, double& a_y, double& a_width)
		{
			auto* pc = ui::PlayerController();
			static auto* lib = ui::Class(L"/Script/UMG.SlateBlueprintLibrary");
			auto* cdo = lib ? lib->GetDefaultObject(false) : nullptr;
			if (!a_w || !pc || !cdo) return false;
			ui::Call geo(a_w, L"GetCachedGeometry");
			const auto gsize = geo.Size("ReturnValue");
			if (!geo || gsize <= 0 || !geo.Run()) return false;
			ui::Call size(cdo, L"GetLocalSize");
			void* g = size.At("Geometry");
			if (!size || !g || size.Size("Geometry") != gsize) return false;
			std::memcpy(g, geo.At("ReturnValue"), static_cast<std::size_t>(gsize));
			size.Run();
			const auto local = size.Get<std::array<double, 2>>("ReturnValue");
			const auto toViewport = [&](double a_lx, double a_ly, double& a_vx, double& a_vy) {
				ui::Call c(cdo, L"LocalToViewport");
				void* gg = c.At("Geometry");
				if (!c || !gg || c.Size("Geometry") != gsize) return false;
				c.Set("WorldContextObject", pc);
				std::memcpy(gg, geo.At("ReturnValue"), static_cast<std::size_t>(gsize));
				const double lc[2] = { a_lx, a_ly };
				c.Set("LocalCoordinate", lc);
				if (!c.RunGuarded()) return false;   // a world-context call: fault-guarded (a quit, a load)
				const auto vpos = c.Get<std::array<double, 2>>("ViewportPosition");
				a_vx = vpos[0];
				a_vy = vpos[1];
				return true;
			};
			double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			if (!toViewport(0.0, 0.0, x0, y0) || !toViewport(local[0], 0.0, x1, y1)) return false;
			a_x = x0;
			a_y = y0;
			a_width = x1 - x0;
			return local[0] > 0.0 && a_width > 1.0;
		}

		void Hide()
		{
			if (!g_shown) return;
			ui::Visible(reflect::Get(g_root), false);
			g_shown = false;
		}
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		if (now < g_nextLook) return;
		g_nextLook = now + 100;

		// which wheel is on screen
		const auto menu = menus::Active();
		const char* name = nullptr;
		if (quickkeys::RadialOpen()) {
			name = wheels::Active() == wheels::Wheel::kMagic ? "Magic Wheel" : "Inventory Wheel";
		} else if (menu == menus::Menu::kInventory && quickkeys::PanelOpen()) {
			name = "Inventory Wheel";
		} else if (menu == menus::Menu::kMagic && pad::MagicPanelUp()) {
			name = "Magic Wheel";
		}
		auto* wheel = name ? quickkeys::VisibleWheel() : nullptr;
		if (!wheel) {
			Hide();
			return;
		}
		if (!reflect::Get(g_root) && !Build()) return;
		auto* root = reflect::Get(g_root);
		auto* label = reflect::Get(g_label);
		auto* slot = reflect::Get(g_slot);
		if (!root || !label || !slot) return;
		double x = 0, y = 0, w = 0;
		if (!Measure(wheel, x, y, w)) {
			Hide();
			return;
		}
		if (g_text != name) {
			SetText(label, name);
			g_text = name;
		}
		const double px = x + w * 0.5, py = y - 6.0;   // centred over the wheel, just above its top
		if (std::abs(px - g_x) > 0.5 || std::abs(py - g_y) > 0.5) {
			ui::Vec2(slot, L"SetPosition", px, py);
			g_x = px, g_y = py;
		}
		if (!g_shown) {
			ui::Visible(root, true);
			g_shown = true;
		}
	}
}
