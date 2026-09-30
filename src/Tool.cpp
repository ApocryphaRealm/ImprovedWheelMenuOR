// The TestBench driving tool (rule 64): iwm.pad. Runs on TestBench's listener thread; every read of game state is
// handed to the game thread (tool::Pump, from the controller read) and waited for.
#include "Tool.h"

#include "Ammo.h"
#include "Inventory.h"
#include "Menus.h"
#include "Pad.h"
#include "QuickKeys.h"
#include "Rows.h"
#include "TestBenchAPI.h"
#include "Wheels.h"

#include <Xinput.h>   // constants only

#include <cmath>
#include <condition_variable>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace tool
{
	namespace
	{
		TestBenchAPI::ITestBenchInterface001* g_tb = nullptr;

		std::mutex              g_lock;
		std::condition_variable g_cv;
		bool                    g_wanted = false;
		bool                    g_done = false;
		json                    g_answer;

		void Write(void* a_sink, TestBenchAPI::WriteFn a_write, const json& a_j) { a_write(a_sink, a_j.dump().c_str()); }

		WORD ButtonBit(const std::string& a_name)
		{
			static const std::pair<const char*, WORD> kButtons[] = {
				{ "a", XINPUT_GAMEPAD_A }, { "b", XINPUT_GAMEPAD_B }, { "x", XINPUT_GAMEPAD_X }, { "y", XINPUT_GAMEPAD_Y },
				{ "lb", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "rb", XINPUT_GAMEPAD_RIGHT_SHOULDER },
				{ "l3", XINPUT_GAMEPAD_LEFT_THUMB }, { "r3", XINPUT_GAMEPAD_RIGHT_THUMB },
				{ "up", XINPUT_GAMEPAD_DPAD_UP }, { "down", XINPUT_GAMEPAD_DPAD_DOWN },
				{ "left", XINPUT_GAMEPAD_DPAD_LEFT }, { "right", XINPUT_GAMEPAD_DPAD_RIGHT },
				{ "start", XINPUT_GAMEPAD_START }, { "back", XINPUT_GAMEPAD_BACK },
			};
			for (const auto& [n, bit] : kButtons) {
				if (a_name == n) {
					return bit;
				}
			}
			return 0;
		}

		// the right stick pointing at a key: the wheel draws key 0 (UI "1") at the top, then clockwise, 45 degrees each
		// (the inventory panel read 2026-09-29)
		std::pair<SHORT, SHORT> StickFor(int a_key)
		{
			constexpr double kPi = 3.14159265358979323846;
			const double deg = 90.0 - 45.0 * a_key;   // key 0 = straight up, key 2 = right
			return { static_cast<SHORT>(30000 * std::cos(deg * kPi / 180.0)), static_cast<SHORT>(30000 * std::sin(deg * kPi / 180.0)) };
		}

		json State()
		{
			json j;
			const auto menu = menus::Active();
			j["menu"] = menus::Name(menu);
			j["radial_open"] = quickkeys::RadialOpen();
			j["panel_open"] = quickkeys::PanelOpen() || (menu == menus::Menu::kMagic && pad::MagicPanelUp());
			j["pointed"] = quickkeys::PointedSlot();
			j["active_wheel"] = wheels::Name(wheels::Active());
			j["queued_steps"] = pad::Queued();
			j["rows"] = rows::Status();
			j["wheels"] = wheels::Status();
			const auto keys = inventory::Keys();
			json k = json::array();
			for (const auto id : keys) {
				k.push_back(id ? inventory::NameOf(id) : "");
			}
			j["game_keys"] = k;
			const auto equip = wheels::Describe(wheels::Wheel::kEquipment);
			const auto magic = wheels::Describe(wheels::Wheel::kMagic);
			j["equipment"] = json(std::vector<std::string>(equip.begin(), equip.end()));
			j["magic"] = json(std::vector<std::string>(magic.begin(), magic.end()));
			int drawn = 0;
			for (auto* i : quickkeys::ReadIcons()) {
				drawn += i ? 1 : 0;
			}
			j["pictures_drawn"] = drawn;
			j["ammo_wheel"] = ammo::State();
			return j;
		}

		void Tool(void*, const char* a_args, void* a_sink, TestBenchAPI::WriteFn a_write)
		{
			json args = json::parse(a_args ? a_args : "{}", nullptr, false);
			if (args.is_discarded()) {
				Write(a_sink, a_write, { { "ok", false }, { "error", "args are not JSON" } });
				return;
			}
			const std::string op = args.value("op", "state");
			if (op == "press") {
				// steps: [{buttons:["a"], ms:120, lt:255, rt:0, key:3 (right stick at that key), stick:[x,y]}, {ms:300} (a pause)]
				std::vector<pad::Step> steps;
				for (const auto& s : args.value("steps", json::array())) {
					pad::Step st;
					for (const auto& b : s.value("buttons", json::array())) {
						const WORD bit = ButtonBit(b.get<std::string>());
						if (!bit) {
							Write(a_sink, a_write, { { "ok", false }, { "error", "unknown button " + b.get<std::string>() } });
							return;
						}
						st.buttons |= bit;
					}
					st.lt = static_cast<BYTE>(s.value("lt", 0));
					st.rt = static_cast<BYTE>(s.value("rt", 0));
					if (s.contains("key")) {
						std::tie(st.rx, st.ry) = StickFor(s["key"].get<int>());
					} else if (s.contains("stick")) {
						st.rx = static_cast<SHORT>(s["stick"][0].get<int>());
						st.ry = static_cast<SHORT>(s["stick"][1].get<int>());
					}
					st.ms = s.value("ms", 120);
					steps.push_back(st);
				}
				pad::Queue(steps);
				Write(a_sink, a_write, { { "ok", true }, { "queued", steps.size() } });
				return;
			}
			if (op == "state") {
				std::unique_lock l(g_lock);
				g_wanted = true;
				g_done = false;
				if (!g_cv.wait_for(l, 3s, [] { return g_done; })) {
					g_wanted = false;
					Write(a_sink, a_write, { { "ok", false }, { "error", "the game thread did not answer in 3 s (no controller reads - is the game paused or minimised?)" } });
					return;
				}
				json out = g_answer;
				out["ok"] = true;
				Write(a_sink, a_write, out);
				return;
			}
			Write(a_sink, a_write, { { "ok", false }, { "error", "op: state (default) | press {steps:[{buttons:[a,b,x,y,lb,rb,l3,r3,up,down,left,right,start,back], lt, rt, key|stick, ms}]}" } });
		}
	}

	void Pump()
	{
		{
			std::scoped_lock l(g_lock);
			if (!g_wanted) {
				return;
			}
		}
		json answer = State();
		std::scoped_lock l(g_lock);
		g_answer = std::move(answer);
		g_wanted = false;
		g_done = true;
		g_cv.notify_all();
	}

	bool Register()
	{
		if (g_tb) {
			return true;
		}
		HMODULE tb = ::GetModuleHandleW(L"TestBench.dll");
		auto get = tb ? reinterpret_cast<void* (*)(unsigned)>(::GetProcAddress(tb, "TestBench_GetInterface")) : nullptr;
		g_tb = get ? static_cast<TestBenchAPI::ITestBenchInterface001*>(get(1)) : nullptr;
		if (!g_tb) {
			return false;
		}
		g_tb->RegisterTool("iwm.pad",
			R"({"description":"Improved Wheel Menu driver. op: state (default: menu, radial/panel open, pointed slot, active wheel, the game's keys, both wheels' entries - active marked *) | press {steps:[{buttons:[a|b|x|y|lb|rb|l3|r3|up|down|left|right|start|back], lt, rt, key (right stick at key 0-7) or stick:[x,y], ms}]} laid over the real pad on the game thread","inputSchema":{"type":"object","properties":{"op":{"type":"string"},"steps":{"type":"array"}}},"readOnly":false})",
			&Tool, nullptr);
		logger::info("TestBench tool registered: iwm.pad");
		return true;
	}
}
