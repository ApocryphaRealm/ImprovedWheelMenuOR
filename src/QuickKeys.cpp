#include "QuickKeys.h"

#include "Menus.h"
#include "PEHook.h"


namespace quickkeys
{
	namespace
	{
		// The Blueprint class of the radial and its view model's native class, as the running game names them.
		constexpr const wchar_t* kWidgetClassPath = L"/Game/UI/Modern/GameMenuLayer/WBP_ModernMenu_QuickKeys.WBP_ModernMenu_QuickKeys_C";
		constexpr const wchar_t* kViewModelClassPath = L"/Script/Altar.VQuickKeysMenuViewModel";
		constexpr std::ptrdiff_t kKeyIndexOffset = 0xD0;   // VQuickKeysMenuViewModel::KeyIndex (IntProperty), read by reflection 2026-09-26


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

		std::int32_t* KeyIndexField()
		{
			auto* vm = g_viewModel.load(std::memory_order_acquire);
			auto* cls = g_viewModelClass.load(std::memory_order_acquire);
			if (!vm || !IsLive(vm, cls)) {
				vm = FindViewModel(cls);   // recreated after a load? find it again
				g_viewModel.store(vm, std::memory_order_release);
				if (!vm) {
					return nullptr;
				}
			}
			return reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uint8_t*>(vm) + kKeyIndexOffset);
		}

		int ReadKeyIndex()
		{
			const auto* f = KeyIndexField();
			return f ? *f : -1;
		}

		// The instance that opened as a menu's panel (nullptr when none) - its close is the panel's, not the radial's.
		std::atomic<UE::UObject*> g_panelObject{ nullptr };

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
				case Event::kPanelOpened:
					g_status.panelOpen = true;
					g_status.pointedSlot = -1;
					break;
				case Event::kPointed:
					g_status.pointedSlot = a_slot;
					break;
				case Event::kClosed:
					// the widget collapses itself once as the menu layer is built (no open before it): not the
					// player closing the radial, and it chose nothing
					if (!g_status.open) {
						logger::debug("quick keys: collapsed while not open (widget set-up) - ignored");
						return;
					}
					g_status.open = false;
					g_status.lastChosenSlot = a_slot;
					break;
				case Event::kPanelClosed:
					if (!g_status.panelOpen) {
						return;
					}
					g_status.panelOpen = false;
					break;
				}
			}
			if (g_listener) {
				g_listener(a_event, a_slot);
			}
		}

		void OnVisibility(UE::UObject* a_obj, std::uint8_t a_vis)
		{
			if (a_vis == 4) {   // SelfHitTestInvisible: shown
				if (menus::Active() != menus::Menu::kNone) {
					g_panelObject.store(a_obj);
					Emit(Event::kPanelOpened, -1);
				} else {
					Emit(Event::kOpened, -1);
				}
			} else if (a_vis == 1) {   // Collapsed: hidden
				if (g_panelObject.load() == a_obj) {
					g_panelObject.store(nullptr);
					Emit(Event::kPanelClosed, -1);
				} else {
					Emit(Event::kClosed, ReadKeyIndex());
				}
			}
		}

		// pe::Watch handler: runs on the game thread before the widget's function
		void OnWidgetEvent(UE::UObject* a_obj, UE::UFunction* a_fn, void* a_params)
		{
			if (a_fn == g_fnVisibility) {
				if (a_params) { OnVisibility(a_obj, *static_cast<const std::uint8_t*>(a_params)); }
			} else if (a_fn == g_fnKeyIndex) {
				if (a_params) { Emit(Event::kPointed, *static_cast<const std::int32_t*>(a_params)); }
			} else if (!g_fnVisibility || !g_fnKeyIndex) {
				// still learning the two UFunction pointers: one name compare per unknown function
				const std::string n = pe::FunctionName(a_fn);
				if (!g_fnVisibility && n == "OnVisibilityChangedEvent") {
					g_fnVisibility = a_fn;
					if (a_params) { OnVisibility(a_obj, *static_cast<const std::uint8_t*>(a_params)); }
				} else if (!g_fnKeyIndex && n == "Update Key Index") {
					g_fnKeyIndex = a_fn;
					if (a_params) { Emit(Event::kPointed, *static_cast<const std::int32_t*>(a_params)); }
				}
			}
		}
	}

	void Install(Listener a_listener)
	{
		g_listener = a_listener;
	}

	// From the plugin's lazy thread until the watch is in (the class exists at the main menu).
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
				return;
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
		const bool hooked = pe::Watch(widgetClass, &OnWidgetEvent);
		std::scoped_lock l(g_statusLock);
		g_status.widgetClassFound = true;
		g_status.viewModelFound = g_viewModel.load(std::memory_order_relaxed) != nullptr;
		g_status.hookInstalled = hooked;
		if (hooked) {
			g_status.problem.clear();
		} else {
			g_status.problem = "the widget's vtable could not be swapped";
		}
	}

	void CancelChoice()
	{
		if (auto* f = KeyIndexField()) {
			logger::info("quick keys: choice cancelled (the view model pointed at slot {})", *f + 1);
			*f = -1;
		}
		std::scoped_lock l(g_statusLock);
		g_status.pointedSlot = -1;
	}

	bool RadialOpen()
	{
		std::scoped_lock l(g_statusLock);
		return g_status.open;
	}

	bool PanelOpen()
	{
		std::scoped_lock l(g_statusLock);
		return g_status.panelOpen;
	}

	int PointedSlot()
	{
		std::scoped_lock l(g_statusLock);
		return g_status.pointedSlot;
	}

	Status GetStatus()
	{
		std::scoped_lock l(g_statusLock);
		return g_status;
	}
}
