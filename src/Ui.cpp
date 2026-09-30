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

	bool GuardedProcessEvent(UE::UObject* a_obj, UE::UFunction* a_fn, void* a_params)
	{
		__try {
			a_obj->ProcessEvent(a_fn, a_params);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool Call::RunGuarded() { return m_fn && m_obj && GuardedProcessEvent(m_obj, m_fn, m_params.data()); }

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

	UE::UObject* Load(const wchar_t* a_path)
	{
		if (!a_path) return nullptr;
		if (auto* o = UE::StaticFindObject<UE::UObject>(nullptr, nullptr, a_path)) return o;
		static auto* lib = Class(L"/Script/Engine.KismetSystemLibrary");
		auto*        cdo = lib ? lib->GetDefaultObject(false) : nullptr;
		if (!cdo) return nullptr;
		Call mk(cdo, L"MakeSoftObjectPath");
		Call conv(cdo, L"Conv_SoftObjPathToSoftObjRef");
		Call load(cdo, L"LoadAsset_Blocking");
		void*      path = mk.At("PathString");
		const auto pathSize = mk.Size("ReturnValue");
		const auto refSize = conv.Size("ReturnValue");
		if (!mk || !conv || !load || !path || pathSize <= 0 || refSize <= 0 || conv.Size("SoftObjectPath") != pathSize || load.Size("Asset") != refSize) {
			return nullptr;
		}
		// the FString is built in place and never destroyed - a small, one-time leak per asset (as in Minimap Menu)
		new (path) UE::FString(a_path);
		mk.Run();
		std::memcpy(conv.At("SoftObjectPath"), mk.At("ReturnValue"), static_cast<std::size_t>(pathSize));
		conv.Run();
		std::memcpy(load.At("Asset"), conv.At("ReturnValue"), static_cast<std::size_t>(refSize));
		load.Run();
		return load.Get<UE::UObject*>("ReturnValue");
	}

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
		if (!c.RunGuarded()) return nullptr;   // a world-context call: fault-guarded (a quit, a load)
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

namespace ui
{
	UE::UObject* ChildNamed(UE::UObject* a_userWidget, const wchar_t* a_name)
	{
		if (!a_userWidget || !a_name) return nullptr;
		Call c(a_userWidget, L"GetWidgetFromName");
		if (!c) return nullptr;
		c.Set("Name", UE::FName(a_name, UE::EFindName::Find));
		return c.Run() ? c.Get<UE::UObject*>("ReturnValue") : nullptr;
	}

	UE::UObject* BrushResource(UE::UObject* a_image)
	{
		static auto* brushStruct = reinterpret_cast<UE::UStruct*>(UE::StaticFindObject<UE::UObject>(nullptr, nullptr, L"/Script/SlateCore.SlateBrush"));
		if (!a_image || !brushStruct || !reflect::Ok()) return nullptr;
		const auto brushOff = reflect::Offset(reinterpret_cast<UE::UStruct*>(a_image->GetClass()), "Brush");
		const auto resOff = reflect::Offset(brushStruct, "ResourceObject");
		if (brushOff < 0 || resOff < 0) return nullptr;
		return *reinterpret_cast<UE::UObject* const*>(reinterpret_cast<const std::uint8_t*>(a_image) + brushOff + resOff);
	}

	float RenderOpacity(UE::UObject* a_widget)
	{
		static auto* widget = Class(L"/Script/UMG.Widget");
		const auto off = widget && a_widget && reflect::Ok() ? reflect::Offset(reinterpret_cast<UE::UStruct*>(widget), "RenderOpacity") : -1;
		return off >= 0 ? *reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(a_widget) + off) : 1.0f;
	}

	bool Measure(UE::UObject* a_w, double& a_x, double& a_y, double& a_width, double& a_height)
	{
		auto* pc = PlayerController();
		static auto* lib = Class(L"/Script/UMG.SlateBlueprintLibrary");
		auto* cdo = lib ? lib->GetDefaultObject(false) : nullptr;
		if (!a_w || !pc || !cdo) return false;
		Call geo(a_w, L"GetCachedGeometry");
		const auto gsize = geo.Size("ReturnValue");
		if (!geo || gsize <= 0 || !geo.Run()) return false;
		Call size(cdo, L"GetLocalSize");
		void* g = size.At("Geometry");
		if (!size || !g || size.Size("Geometry") != gsize) return false;
		std::memcpy(g, geo.At("ReturnValue"), static_cast<std::size_t>(gsize));
		size.Run();
		const auto local = size.Get<std::array<double, 2>>("ReturnValue");
		const auto toViewport = [&](double a_lx, double a_ly, double& a_vx, double& a_vy) {
			Call c(cdo, L"LocalToViewport");
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
		if (!toViewport(0.0, 0.0, x0, y0) || !toViewport(local[0], local[1], x1, y1)) return false;
		a_x = x0;
		a_y = y0;
		a_width = x1 - x0;
		a_height = y1 - y0;
		return local[0] > 0.0 && a_width > 1.0 && a_height > 1.0;
	}

	float MidScalar(UE::UObject* a_mid, const wchar_t* a_name)
	{
		Call c(a_mid, L"K2_GetScalarParameterValue");
		if (!c) return 0.0f;
		c.Set("ParameterName", UE::FName(a_name, UE::EFindName::Add));
		return c.Run() ? c.Get<float>("ReturnValue") : 0.0f;
	}

	void SetMidScalar(UE::UObject* a_mid, const wchar_t* a_name, float a_v)
	{
		Call c(a_mid, L"SetScalarParameterValue");
		if (!c) return;
		c.Set("ParameterName", UE::FName(a_name, UE::EFindName::Add));
		c.Set("Value", a_v);
		c.Run();
	}

	void SetMidTexture(UE::UObject* a_mid, const wchar_t* a_name, UE::UObject* a_texture)
	{
		Call c(a_mid, L"SetTextureParameterValue");
		if (!c) return;
		c.Set("ParameterName", UE::FName(a_name, UE::EFindName::Add));
		c.Set("Value", a_texture);
		c.Run();
	}
}
