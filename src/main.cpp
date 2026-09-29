// Improved Wheel Menu (Oblivion Remastered) - entry point.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "QuickKeys.h"
#include "Settings.h"

namespace
{
	const char* SlotWord(int a_slot)
	{
		// KeyIndex numbering (runtime research 2026-09-26): clockwise, 1 at the top.
		static constexpr const char* kWords[8] = { "top-left", "top", "top-right", "right", "bottom-right", "bottom", "bottom-left", "left" };
		return a_slot >= 0 && a_slot < 8 ? kWords[a_slot] : "none";
	}

	// The plain-text self-check beside the log: what installed, what fired last (memory: guards-and-a-selfcheck-
	// report-in-every-build). Rewritten on every event, so a test can read it without the log.
	void WriteSelfCheck()
	{
		const auto s = quickkeys::GetStatus();
		const auto path = settings::PluginFolder() / L"ImprovedWheelMenu.selfcheck.txt";
		std::string text = std::format(
			"Improved Wheel Menu {} self-check\n"
			"widget class found: {}\nview model found: {}\nProcessEvent hook: {}\n"
			"radial open now: {}\npointed slot: {} ({})\nlast chosen slot: {} ({})\nopens this session: {}\nproblem: {}\n",
			IWM_VERSION, s.widgetClassFound ? "yes" : "no", s.viewModelFound ? "yes" : "no", s.hookInstalled ? "installed" : "NOT installed",
			s.open ? "yes" : "no", s.pointedSlot, SlotWord(s.pointedSlot), s.lastChosenSlot, SlotWord(s.lastChosenSlot), s.opens,
			s.problem.empty() ? "none" : s.problem);
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
			logger::info("radial opened");
			break;
		case quickkeys::Event::kPointed:
			logger::debug("radial points at slot {} ({})", a_slot, SlotWord(a_slot));
			break;
		case quickkeys::Event::kClosed:
			logger::info("radial closed; the game uses slot {} ({})", a_slot, SlotWord(a_slot));
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
		// the self-check is written once now (nothing installed yet) and again as the hook lands
		WriteSelfCheck();
		std::thread([] {
			for (int i = 0; i < 600; ++i) {   // ten minutes at most: the hook lands when a save is loaded
				std::this_thread::sleep_for(1s);
				if (quickkeys::GetStatus().hookInstalled) {
					WriteSelfCheck();
					return;
				}
			}
			WriteSelfCheck();
		}).detach();
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
