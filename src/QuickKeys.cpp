#include "QuickKeys.h"

#include <MinHook.h>

namespace quickkeys
{
	namespace
	{
		// The Blueprint class of the radial and its view model's native class, as the running game names them.
		constexpr const wchar_t* kWidgetClassPath = L"/Game/UI/Modern/GameMenuLayer/WBP_ModernMenu_QuickKeys.WBP_ModernMenu_QuickKeys_C";
		constexpr const wchar_t* kViewModelClassPath = L"/Script/Altar.VQuickKeysMenuViewModel";
		constexpr std::size_t    kProcessEventSlot = 0x4D;
		constexpr std::ptrdiff_t kKeyIndexOffset = 0xD0;   // VQuickKeysMenuViewModel::KeyIndex (IntProperty), read by reflection 2026-09-26

		using ProcessEvent_t = void (*)(UE::UObject*, UE::UFunction*, void*);
		ProcessEvent_t g_original = nullptr;

		std::atomic<UE::UClass*>  g_widgetClass{ nullptr };
		std::atomic<UE::UObject*> g_viewModel{ nullptr };
		std::atomic<UE::UClass*>  g_viewModelClass{ nullptr };
		Listener                  g_listener = nullptr;

		std::mutex g_statusLock;
		Status     g_status;

		// The two UFunction names we act on, compared once per distinct UFunction* and remembered.
		UE::UFunction* g_fnVisibility = nullptr;
		UE::UFunction* g_fnKeyIndex = nullptr;
		std::atomic<int> g_frame{ 0 };

		std::string Utf8(const UE::FString& a_s)
		{
			const wchar_t* d = UE::GetData(a_s);
			const int      n = UE::GetNum(a_s);
			if (!d || n <= 0) {
				return {};
			}
			const int len = d[n - 1] == L'\0' ? n - 1 : n;
			const int bytes = WideCharToMultiByte(CP_UTF8, 0, d, len, nullptr, 0, nullptr, nullptr);
			std::string out(bytes > 0 ? static_cast<std::size_t>(bytes) : 0, '\0');
			if (bytes > 0) {
				WideCharToMultiByte(CP_UTF8, 0, d, len, out.data(), bytes, nullptr, nullptr);
			}
			return out;
		}

		std::string NameOf(const UE::FName& a_n) { return Utf8(a_n.ToString()); }

		void SetProblem(const std::string& a_why)
		{
			std::scoped_lock l(g_statusLock);
			g_status.problem = a_why;
		}

		// A pointer is the live view model only if the object array still lists it at its own index with that class.
		bool IsLive(UE::UObject* a_o, UE::UClass* a_class)
		{
			auto* arr = UE::FUObjectArray::GetSingleton();
			if (!a_o || !arr) {
				return false;
			}
			const std::int32_t idx = a_o->internalIndex;
			if (idx < 0 || idx >= arr->GetObjectArrayNum()) {
				return false;
			}
			auto* item = arr->IndexToObject(idx);
			return item && reinterpret_cast<UE::UObject*>(item->object) == a_o && a_o->GetClass() == a_class;
		}

		// The one transient view model instance (not the class default object) - found by walking the object array.
		UE::UObject* FindViewModel(UE::UClass* a_class)
		{
			auto* arr = UE::FUObjectArray::GetSingleton();
			if (!arr || !a_class) {
				return nullptr;
			}
			UE::UObject* found = nullptr;
			arr->LockInternalArray();
			const std::int32_t n = arr->GetObjectArrayNum();
			for (std::int32_t i = 0; i < n && !found; ++i) {
				auto* item = arr->IndexToObject(i);
				if (!item || !item->object) {
					continue;
				}
				auto* o = reinterpret_cast<UE::UObject*>(item->object);
				if (o->GetClass() == a_class && o != a_class->GetDefaultObject(false)) {
					found = o;
				}
			}
			arr->UnlockInternalArray();
			return found;
		}

		int ReadKeyIndex()
		{
			auto* vm = g_viewModel.load(std::memory_order_acquire);
			auto* cls = g_viewModelClass.load(std::memory_order_acquire);
			if (!vm || !IsLive(vm, cls)) {
				vm = FindViewModel(cls);   // recreated after a load? find it again
				g_viewModel.store(vm, std::memory_order_release);
				if (!vm) {
					return -1;
				}
			}
			return *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(vm) + kKeyIndexOffset);
		}

		void Emit(Event a_event, int a_slot)
		{
			{
				std::scoped_lock l(g_statusLock);
				switch (a_event) {
				case Event::kOpened:
					g_status.open = true;
					g_status.pointedSlot = -1;
					++g_status.opens;
					break;
				case Event::kPointed:
					g_status.pointedSlot = a_slot;
					break;
				case Event::kClosed:
					g_status.open = false;
					g_status.lastChosenSlot = a_slot;
					break;
				}
			}
			if (g_listener) {
				g_listener(a_event, a_slot);
			}
		}

		void HookedProcessEvent(UE::UObject* a_obj, UE::UFunction* a_fn, void* a_params)
		{
			if (a_obj && a_fn && a_obj->GetClass() == g_widgetClass.load(std::memory_order_relaxed)) {
				if (a_fn == g_fnVisibility) {
					if (a_params) {
						const auto vis = *static_cast<const std::uint8_t*>(a_params);   // ESlateVisibility
						if (vis == 4) {                                                   // SelfHitTestInvisible: shown
							Emit(Event::kOpened, -1);
						} else if (vis == 1) {                                            // Collapsed: hidden
							Emit(Event::kClosed, ReadKeyIndex());
						}
					}
				} else if (a_fn == g_fnKeyIndex) {
					if (a_params) {
						Emit(Event::kPointed, *static_cast<const std::int32_t*>(a_params));
					}
				} else if (!g_fnVisibility || !g_fnKeyIndex) {
					// still learning the two UFunction pointers: one name compare per unknown function
					const std::string n = NameOf(a_fn->GetFName());
					if (!g_fnVisibility && n == "OnVisibilityChangedEvent") {
						g_fnVisibility = a_fn;
						if (a_params) {
							const auto vis = *static_cast<const std::uint8_t*>(a_params);
							if (vis == 4) { Emit(Event::kOpened, -1); } else if (vis == 1) { Emit(Event::kClosed, ReadKeyIndex()); }
						}
					} else if (!g_fnKeyIndex && n == "Update Key Index") {
						g_fnKeyIndex = a_fn;
						if (a_params) { Emit(Event::kPointed, *static_cast<const std::int32_t*>(a_params)); }
					}
				}
			}
			if (g_original) {
				g_original(a_obj, a_fn, a_params);
			}
		}

		bool InstallHook(UE::UClass* a_widgetClass)
		{
			// The class default object shares the instances' vtable: its ProcessEvent slot is the function to hook.
			auto* cdo = a_widgetClass ? a_widgetClass->GetDefaultObject(false) : nullptr;
			if (!cdo) {
				SetProblem("widget class has no default object yet");
				return false;
			}
			void** vtable = *reinterpret_cast<void***>(cdo);
			if (!vtable || !vtable[kProcessEventSlot]) {
				SetProblem("widget vtable unreadable");
				return false;
			}
			void* target = vtable[kProcessEventSlot];
			const MH_STATUS init = MH_Initialize();   // shared with any other MinHook user in-process
			if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
				SetProblem(std::format("MH_Initialize: {}", static_cast<int>(init)));
				return false;
			}
			void* original = nullptr;
			const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void*>(&HookedProcessEvent), &original);
			if (created != MH_OK || MH_EnableHook(target) != MH_OK) {
				SetProblem(std::format("MinHook refused ProcessEvent at {:p} ({})", target, static_cast<int>(created)));
				return false;
			}
			g_original = reinterpret_cast<ProcessEvent_t>(original);
			logger::info("quick keys: ProcessEvent hooked at {:p} (widget class {:p})", target, static_cast<void*>(a_widgetClass));
			return true;
		}
	}

	void Install(Listener a_listener)
	{
		g_listener = a_listener;
		// Nothing of ours runs per frame (no menu, no drawing), so a thread looks for the widget's class once a
		// second until it exists and installs the hook then. It ends itself when the hook is in.
		std::thread([] {
			while (!GetStatus().hookInstalled) {
				std::this_thread::sleep_for(1s);
				Tick();
			}
		}).detach();
	}

	void Tick()
	{
		if (GetStatus().hookInstalled) {
			return;
		}
		g_frame.fetch_add(1, std::memory_order_relaxed);
		auto* widgetClass = g_widgetClass.load(std::memory_order_acquire);
		if (!widgetClass) {
			widgetClass = UE::StaticFindObject<UE::UClass>(nullptr, nullptr, kWidgetClassPath);
			if (!widgetClass) {
				return;   // not loaded yet - main menu
			}
			g_widgetClass.store(widgetClass, std::memory_order_release);
			logger::info("quick keys: widget class found ({})", Utf8(widgetClass->GetFullName()));
		}
		auto* vmClass = g_viewModelClass.load(std::memory_order_acquire);
		if (!vmClass) {
			vmClass = UE::StaticFindObject<UE::UClass>(nullptr, nullptr, kViewModelClassPath);
			if (vmClass) {
				g_viewModelClass.store(vmClass, std::memory_order_release);
				g_viewModel.store(FindViewModel(vmClass), std::memory_order_release);
			}
		}
		const bool hooked = InstallHook(widgetClass);
		std::scoped_lock l(g_statusLock);
		g_status.widgetClassFound = true;
		g_status.viewModelFound = g_viewModel.load(std::memory_order_relaxed) != nullptr;
		g_status.hookInstalled = hooked;
		if (hooked) {
			g_status.problem.clear();
		}
	}

	Status GetStatus()
	{
		std::scoped_lock l(g_statusLock);
		return g_status;
	}
}
