// Improved Wheel Menu (Oblivion Remastered) - entry point.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Menus.h"
#include "Pad.h"
#include "QuickKeys.h"
#include "Rows.h"
#include "Tool.h"
#include "Settings.h"
#include "Wheels.h"

namespace
{
	const char* SlotWord(int a_slot)
	{
		// KeyIndex is the slot number as drawn, 1 at the top, clockwise (the game's key = number - 1; read 2026-09-29 -
		// the 2026-09-26 note had it starting at the top-left)
		static constexpr const char* kWords[9] = { "none", "top", "top-right", "right", "bottom-right", "bottom", "bottom-left", "left", "top-left" };
		return a_slot >= 0 && a_slot <= 8 ? kWords[a_slot] : "none";
	}

	// The plain-text self-check beside the log: what installed, what fired last (memory: guards-and-a-selfcheck-
	// report-in-every-build). Rewritten on every event, so a test can read it without the log.
	void WriteSelfCheck()
	{
		const auto s = quickkeys::GetStatus();
		const auto p = pad::GetStatus();
		const auto path = settings::PluginFolder() / L"ImprovedWheelMenu.selfcheck.txt";
		std::string text = std::format(
			"Improved Wheel Menu {} self-check\n"
			"widget class found: {}\nview model found: {}\nquick keys watch: {}\n"
			"radial open now: {}\nmenu panel open now: {}\npointed slot: {} ({})\nlast chosen slot: {} ({})\nopens this session: {}\n"
			"controller rules: {} (reads {}, rewritten {}, chained after {})\nmenu now: {}\nactive wheel: {}\nmenu rows: {}\nwheels: {}\nproblem: {}\n",
			IWM_VERSION, s.widgetClassFound ? "yes" : "no", s.viewModelFound ? "yes" : "no", s.hookInstalled ? "installed" : "NOT installed",
			s.open ? "yes" : "no", s.panelOpen ? "yes" : "no", s.pointedSlot, SlotWord(s.pointedSlot), s.lastChosenSlot, SlotWord(s.lastChosenSlot), s.opens,
			p.installed ? "installed" : "NOT installed", p.reads, p.rewritten, p.previousTarget.empty() ? "-" : p.previousTarget,
			menus::Name(menus::Active()), wheels::Name(wheels::Active()), rows::Status(), wheels::Status(), s.problem.empty() ? "none" : s.problem);
		FILE* f = nullptr;
		if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
			std::fwrite(text.data(), 1, text.size(), f);
			std::fclose(f);
		}
	}

	void OnQuickKeys(quickkeys::Event a_event, int a_slot)
	{
		switch (a_event) {
		case quickkeys::Event::kOpened:
			logger::info("radial opened (the {} wheel)", wheels::Name(wheels::Active()));
			wheels::RadialShown();
			break;
		case quickkeys::Event::kPointed:
			logger::debug("points at slot {} ({})", a_slot, SlotWord(a_slot));
			break;
		case quickkeys::Event::kClosed:
			logger::info("radial closed; the game uses slot {} ({})", a_slot, SlotWord(a_slot));
			wheels::RadialHidden();
			break;
		case quickkeys::Event::kPanelOpened:
			logger::info("assign panel opened in the {}", menus::Name(menus::Active()));
			wheels::PanelShown(menus::Active());
			break;
		case quickkeys::Event::kPanelClosed:
			logger::info("assign panel closed");
			wheels::PanelHidden();
			break;
		}
		WriteSelfCheck();
	}

	void OnMessage(OBSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg || a_msg->type != OBSE::MessagingInterface::kPostLoad) {
			return;
		}
		quickkeys::Install(&OnQuickKeys);
		// The widget and menu classes appear as the game builds them (the quick keys widget at the main menu, each
		// menu the first time it opens). They are looked for on the GAME thread, from the controller read, about
		// every 200 ms - never from a thread of our own (a start-up crashed in UObjectArray, 2026-09-29).
		pad::SetFrameCallback([] {
			tool::Pump();
			static auto next = std::chrono::steady_clock::now();
			static bool reported = false;
			static bool toolRegistered = false;
			const auto now = std::chrono::steady_clock::now();
			if (now < next) {
				return;
			}
			next = now + 200ms;
			quickkeys::Tick();
			menus::Tick();
			rows::Tick();
			if (!toolRegistered) {
				toolRegistered = tool::Register();
			}
			if (!reported && quickkeys::GetStatus().hookInstalled) {
				reported = true;
				WriteSelfCheck();
			}
		});
		pad::Install();
		WriteSelfCheck();
	}
}

OBSE_PLUGIN_LOAD(const OBSE::LoadInterface* a_obse)
{
	OBSE::Init(a_obse);
	settings::Load();
	{
		const auto level = static_cast<spdlog::level::level_enum>(std::clamp(settings::Get().logLevel, 0, 4));
		logger::set_level(level, level);
	}
	logger::info("Improved Wheel Menu {} loaded (Oblivion Remastered)", IWM_VERSION);

	if (auto* messaging = OBSE::GetMessagingInterface()) {
		if (!messaging->RegisterListener(&OnMessage)) {
			logger::error("OBSE messaging: listener refused - the quick keys hook will not be installed");
		}
	} else {
		logger::error("OBSE messaging interface missing - the quick keys hook will not be installed");
	}
	return true;
}
