#include "Ui.h"

#include "Reflect.h"

namespace ui
{
	namespace
	{
		constexpr std::ptrdiff_t kFieldNext = 0x18;
		constexpr std::ptrdiff_t kFieldName = 0x20;
		constexpr std::ptrdiff_t kElementSize = 0x34;

		std::string Narrow(const UE::FName& a_n)
		{
			const auto s = a_n.ToString();
			const wchar_t* d = UE::GetData(s);
			std::string out;
			for (int i = 0, n = UE::GetNum(s); d && i < n && d[i]; ++i) out.push_back(d[i] < 0x80 ? static_cast<char>(d[i]) : '?');
			return out;
		}
	}

	std::int32_t SizeOf(UE::UStruct* a_struct, std::string_view a_name)
	{
		for (UE::UStruct* s = a_struct; s; s = s->superStruct) {
			for (auto* f = reinterpret_cast<std::uint8_t*>(s->childProperties); f; f = *reinterpret_cast<std::uint8_t**>(f + kFieldNext)) {
				if (Narrow(*reinterpret_cast<const UE::FName*>(f + kFieldName)) == a_name) {
					return *reinterpret_cast<const std::int32_t*>(f + kElementSize);
				}
			}
		}
		return -1;
	}

	Call::Call(UE::UObject* a_obj, const wchar_t* a_fn) :
		m_obj(a_obj),
		m_fn(a_obj ? a_obj->FindFunction(UE::FName(a_fn, UE::EFindName::Find)) : nullptr)
	{
		if (m_fn) m_params.assign(static_cast<std::size_t>(std::max(reinterpret_cast<UE::UStruct*>(m_fn)->propertiesSize, 0)) + 16, 0);
	}

	void* Call::At(std::string_view a_name)
	{
		if (!m_fn) return nullptr;
		const auto off = reflect::Offset(reinterpret_cast<UE::UStruct*>(m_fn), a_name);
		return off >= 0 ? m_params.data() + off : nullptr;
	}

	bool Call::Run()
	{
		if (!m_fn || !m_obj) return false;
		m_obj->ProcessEvent(m_fn, m_params.data());
		return true;
	}

	bool CallFirst(UE::UObject* a_obj, const wchar_t* a_fn, const void* a_bytes, std::size_t a_size)
	{
		auto* fn = a_obj ? a_obj->FindFunction(UE::FName(a_fn, UE::EFindName::Find)) : nullptr;
		auto* st = reinterpret_cast<UE::UStruct*>(fn);
		auto* f = st ? reinterpret_cast<std::uint8_t*>(st->childProperties) : nullptr;
		const auto off = f ? *reinterpret_cast<const std::int32_t*>(f + 0x44) : -1;
		if (off < 0) return false;
		std::vector<std::uint8_t> params(static_cast<std::size_t>(std::max(st->propertiesSize, 0)) + a_size + 16, 0);
		std::memcpy(params.data() + off, a_bytes, a_size);
		a_obj->ProcessEvent(fn, params.data());
		return true;
	}

	void Vec2(UE::UObject* a_obj, const wchar_t* a_fn, double a_x, double a_y)
	{
		const double v[2] = { a_x, a_y };
		CallFirst(a_obj, a_fn, v, sizeof(v));
	}

	void Float(UE::UObject* a_obj, const wchar_t* a_fn, float a_v) { CallFirst(a_obj, a_fn, &a_v, sizeof(a_v)); }

	void Visible(UE::UObject* a_widget, bool a_visible)
	{
		if (!a_widget) return;
		Call c(a_widget, L"SetVisibility");
		c.Set("InVisibility", static_cast<std::uint8_t>(a_visible ? 3 : 1));
		c.Run();
	}

	void Colour(UE::UObject* a_image, float a_r, float a_g, float a_b, float a_a)
	{
		const float c[4] = { a_r, a_g, a_b, a_a };
		CallFirst(a_image, L"SetColorAndOpacity", c, sizeof(c));
	}

	UE::UClass* Class(const wchar_t* a_path) { return UE::StaticFindObject<UE::UClass>(nullptr, nullptr, a_path); }

	UE::UObject* PlayerController()
	{
		static reflect::Handle cached;
		static ULONGLONG       lastScan = 0;
		if (auto* pc = reflect::Get(cached)) return pc;
		const ULONGLONG now = GetTickCount64();
		if (now - lastScan < 2000) return nullptr;
		lastScan = now;
		// the game's controller is a SUBCLASS (VAltarPlayerController): reflect::Instances matches one class exactly and so
		// never found it - the ammo wheel was never built ("cannot be built yet", every press, 2026-09-29 23:55 on)
		auto* cls = Class(L"/Script/Engine.PlayerController");
		auto* arr = UE::FUObjectArray::GetSingleton();
		UE::UObject* found = nullptr;
		if (cls && arr) {
			arr->LockInternalArray();
			const std::int32_t n = arr->GetObjectArrayNum();
			for (std::int32_t i = 0; i < n && !found; ++i) {
				auto* item = arr->IndexToObject(i);
				auto* o = item ? reinterpret_cast<UE::UObject*>(item->object) : nullptr;
				auto* oc = o ? o->GetClass() : nullptr;
				// not a class default object, not an archetype
				if (oc && oc->IsChildOf(cls) && (static_cast<std::int32_t>(o->objectFlags) & 0x30) == 0) {
					found = o;
				}
			}
			arr->UnlockInternalArray();
		}
		cached = found ? reflect::Hold(found) : reflect::Handle{};
		return found;
	}

	UE::UObject* CreateWidget(const wchar_t* a_classPath)
	{
		static auto* lib = Class(L"/Script/UMG.WidgetBlueprintLibrary");
		auto* cls = Class(a_classPath);
		auto* pc = PlayerController();
		auto* cdo = lib ? lib->GetDefaultObject(false) : nullptr;
		if (!cdo || !cls || !pc) return nullptr;
		Call c(cdo, L"Create");
		c.Set("WorldContextObject", pc);
		c.Set("WidgetType", cls);
		c.Set("OwningPlayer", pc);
		c.Run();
		return c.Get<UE::UObject*>("ReturnValue");
	}

	UE::UObject* RootCanvas(UE::UObject* a_userWidget, const wchar_t* a_name)
	{
		auto* treeClass = Class(L"/Script/UMG.WidgetTree");
		auto* canvasClass = Class(L"/Script/UMG.CanvasPanel");
		if (!a_userWidget || !treeClass || !canvasClass || !a_userWidget->GetClass()) return nullptr;
		auto** tree = reflect::At<UE::UObject*>(a_userWidget, reflect::Offset(a_userWidget->GetClass(), "WidgetTree"));
		if (!tree) return nullptr;
		if (!*tree) *tree = UE::NewObject<UE::UObject>(a_userWidget, treeClass, UE::FName((std::wstring(a_name) + L"Tree").c_str()));
		if (!*tree) return nullptr;
		auto* canvas = UE::NewObject<UE::UObject>(*tree, canvasClass, UE::FName(a_name));
		auto** root = reflect::At<UE::UObject*>(*tree, reflect::Offset(treeClass, "RootWidget"));
		if (!canvas || !root) return nullptr;
		*root = canvas;
		return canvas;
	}

	UE::UObject* AddToCanvas(UE::UObject* a_canvas, const wchar_t* a_class, const wchar_t* a_name, UE::UObject** a_slot)
	{
		*a_slot = nullptr;
		auto* cls = Class(a_class);
		auto* outer = a_canvas ? a_canvas->GetOuter() : nullptr;   // the widget tree
		if (!cls || !outer) return nullptr;
		auto* w = UE::NewObject<UE::UObject>(outer, cls, UE::FName(a_name));
		if (!w) return nullptr;
		Call add(a_canvas, L"AddChildToCanvas");
		add.Set("Content", w);
		add.Run();
		*a_slot = add.Get<UE::UObject*>("ReturnValue");
		if (!*a_slot) return nullptr;
		const bool no = false;
		CallFirst(*a_slot, L"SetAutoSize", &no, sizeof(no));
		return w;
	}

	void Anchors(UE::UObject* a_slot, double a_minX, double a_minY, double a_maxX, double a_maxY)
	{
		const double a[4] = { a_minX, a_minY, a_maxX, a_maxY };
		CallFirst(a_slot, L"SetAnchors", a, sizeof(a));
	}

	bool RoundedBox(UE::UObject* a_image, bool a_circle, const float a_fill[4], const float a_outline[4], float a_width)
	{
		static auto* brushStruct = reinterpret_cast<UE::UStruct*>(UE::StaticFindObject<UE::UObject>(nullptr, nullptr, L"/Script/SlateCore.SlateBrush"));
		static auto* outlineStruct = reinterpret_cast<UE::UStruct*>(UE::StaticFindObject<UE::UObject>(nullptr, nullptr, L"/Script/SlateCore.SlateBrushOutlineSettings"));
		static auto* colorStruct = reinterpret_cast<UE::UStruct*>(UE::StaticFindObject<UE::UObject>(nullptr, nullptr, L"/Script/SlateCore.SlateColor"));
		if (!a_image || !brushStruct || !outlineStruct || !colorStruct || !a_image->GetClass()) return false;
		const auto brushOff = reflect::Offset(a_image->GetClass(), "Brush");
		const auto brushSize = SizeOf(a_image->GetClass(), "Brush");
		const auto drawAs = reflect::Offset(brushStruct, "DrawAs");
		const auto tint = reflect::Offset(brushStruct, "TintColor");
		const auto outline = reflect::Offset(brushStruct, "OutlineSettings");
		const auto radii = reflect::Offset(outlineStruct, "CornerRadii");
		const auto radiiSize = SizeOf(outlineStruct, "CornerRadii");
		const auto oColor = reflect::Offset(outlineStruct, "Color");
		const auto rounding = reflect::Offset(outlineStruct, "RoundingType");
		const auto width = reflect::Offset(outlineStruct, "Width");
		const auto spec = reflect::Offset(colorStruct, "SpecifiedColor");
		const auto rule = reflect::Offset(colorStruct, "ColorUseRule");
		if (brushOff < 0 || brushSize <= 0 || drawAs < 0 || tint < 0 || outline < 0 || radii < 0 || oColor < 0 || rounding < 0 || width < 0 ||
			spec < 0 || rule < 0) {
			return false;
		}
		auto* b = reinterpret_cast<std::uint8_t*>(a_image) + brushOff;
		b[drawAs] = 4;   // RoundedBox
		std::memcpy(b + tint + spec, a_fill, sizeof(float) * 4);
		b[tint + rule] = 0;
		std::memcpy(b + outline + oColor + spec, a_outline, sizeof(float) * 4);
		b[outline + oColor + rule] = 0;
		const double r = 6.0;
		if (radiiSize == 32) {
			const double v[4] = { r, r, r, r };
			std::memcpy(b + outline + radii, v, sizeof(v));
		} else if (radiiSize == 16) {
			const float v[4] = { 6, 6, 6, 6 };
			std::memcpy(b + outline + radii, v, sizeof(v));
		}
		b[outline + rounding] = a_circle ? 1 : 0;
		std::memcpy(b + outline + width, &a_width, sizeof(a_width));
		std::vector<std::uint8_t> copy(b, b + brushSize);
		return CallFirst(a_image, L"SetBrush", copy.data(), copy.size());
	}
}
