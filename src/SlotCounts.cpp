#include "SlotCounts.h"

#include "Reflect.h"
#include "Ui.h"
#include "Wheels.h"

#include <numbers>

namespace slotcounts
{
	namespace
	{
		// where a slot circle's middle sits on the wheel picture, as a share of half its width (measured on the owner's
		// screenshot of 2026-09-30 03:38:46: the slot centres about 0.71 of the way out), and the label just below it
		constexpr double kSlotRadius = 0.71;
		constexpr double kBelow = 0.11;
		constexpr double kTextScale = 0.6;

		reflect::Handle g_root;
		std::array<reflect::Handle, 8> g_label, g_slot;
		std::array<std::string, 8> g_text;
		int       g_builds = 0;
		ULONGLONG g_lastBuild = 0, g_next = 0;
		std::uint32_t g_gen = 0;   // wheels::Generation() at the last text update
		bool      g_shown = false;
		double    g_x = -1e9, g_y = -1e9, g_w = -1;

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
			const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.95f };
			ui::CallFirst(a_label, L"SetShadowColorAndOpacity", shadow, sizeof(shadow));
			ui::Vec2(a_label, L"SetShadowOffset", 1.5, 1.5);
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

		bool Build()
		{
			const ULONGLONG now = GetTickCount64();
			if (g_lastBuild && now - g_lastBuild < 2000) return false;
			g_lastBuild = now;
			auto* root = ui::CreateWidget(L"/Script/UMG.UserWidget");
			const std::wstring n = std::to_wstring(++g_builds);
			auto* canvas = root ? ui::RootCanvas(root, (L"IwmCountsCanvas" + n).c_str()) : nullptr;
			if (!canvas) return false;
			for (int k = 0; k < 8; ++k) {
				UE::UObject* slot = nullptr;
				auto* label = ui::AddToCanvas(canvas, L"/Game/UI/Modern/Prefabs/WBP_AltarTextBlock.WBP_AltarTextBlock_C",
					(L"IwmCount" + n + L"_" + std::to_wstring(k)).c_str(), &slot);
				if (!label || !slot) {
					logger::info("slot counts: the game's text prefab is not loaded yet");
					return false;
				}
				ui::Anchors(slot, 0, 0, 0, 0);
				ui::Vec2(slot, L"SetAlignment", 0.5, 0.5);
				const bool yes = true;
				ui::CallFirst(slot, L"SetAutoSize", &yes, sizeof(yes));
				ui::Vec2(label, L"SetRenderTransformPivot", 0.5, 0.5);
				ui::Vec2(label, L"SetRenderScale", kTextScale, kTextScale);
				g_label[static_cast<std::size_t>(k)] = reflect::Hold(label);
				g_slot[static_cast<std::size_t>(k)] = reflect::Hold(slot);
				g_text[static_cast<std::size_t>(k)].clear();
			}
			ui::Call vp(root, L"AddToViewport");
			vp.Set<std::int32_t>("ZOrder", 61);   // over the wheel and its name
			vp.Run();
			for (auto& h : g_label) White(reflect::Get(h));
			ui::Visible(root, false);
			g_root = reflect::Hold(root);
			g_shown = false;
			g_x = g_y = -1e9, g_w = -1;
			logger::info("slot counts: built (build {})", g_builds);
			return true;
		}
	}

	void Show(UE::UObject* a_gameWheelImage)
	{
		// every 100 ms (the inventory walk is not free) - and at once whenever a wheel changed (Generation moves with
		// every LT / RT step, assign and removal), so the counter follows LT / RT on the read after the press
		const ULONGLONG now = GetTickCount64();
		const auto gen = wheels::Generation();
		if (now < g_next && gen == g_gen) return;
		g_next = now + 100;
		g_gen = gen;
		if (!reflect::Get(g_root) && !Build()) return;
		auto* root = reflect::Get(g_root);
		double x = 0, y = 0, w = 0, h = 0;
		if (!root || !ui::Measure(a_gameWheelImage, x, y, w, h)) {
			Hide();
			return;
		}
		// where: only when the wheel moved or changed size
		if (std::abs(x - g_x) > 0.5 || std::abs(y - g_y) > 0.5 || std::abs(w - g_w) > 0.5) {
			const double half = w * 0.5, cx = x + half, cy = y + h * 0.5;
			for (int k = 0; k < 8; ++k) {
				const double a = k * 45.0 * std::numbers::pi / 180.0;   // slot k+1: 45 degrees apart, clockwise from the top
				const double px = cx + std::sin(a) * kSlotRadius * half;
				const double py = cy - std::cos(a) * kSlotRadius * half + kBelow * half;
				if (auto* s = reflect::Get(g_slot[static_cast<std::size_t>(k)])) ui::Vec2(s, L"SetPosition", px, py);
			}
			g_x = x, g_y = y, g_w = w;
		}
		// what: the shown entry's place among those LT / RT reach, "2/3" ("1/1" for one item); an empty slot shows nothing,
		// a slot whose key holds none of its entries "-/N". Only the labels whose text changed are set.
		const auto counts = wheels::SlotCounts(wheels::Wheel::kEquipment);
		for (int k = 0; k < 8; ++k) {
			const auto& c = counts[static_cast<std::size_t>(k)];
			const std::string text = c.count == 0 ? std::string() : c.position > 0 ? std::format("{}/{}", c.position, c.count) : std::format("-/{}", c.count);
			if (text != g_text[static_cast<std::size_t>(k)]) {
				if (auto* label = reflect::Get(g_label[static_cast<std::size_t>(k)])) SetText(label, text);
				g_text[static_cast<std::size_t>(k)] = text;
			}
		}
		if (!g_shown) {
			ui::Visible(root, true);
			g_shown = true;
		}
	}

	void Hide()
	{
		if (!g_shown) return;
		ui::Visible(reflect::Get(g_root), false);
		g_shown = false;
	}
}
