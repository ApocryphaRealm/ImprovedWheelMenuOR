#pragma once

// ============================================================================================================
// Building a small runtime UMG widget (the ammo wheel): a reflected call with its parameters laid out by NAME from the
// UFunction's own properties (Minimap Menu's / Tween Menu's ue::Call), a call by first parameter, and the objects the
// widget is built from. Game thread only; kept objects are reflect::Handles.
// ============================================================================================================

namespace ui
{
	std::int32_t SizeOf(UE::UStruct* a_struct, std::string_view a_name);   // FProperty::ElementSize (+0x34); -1 when absent

	class Call
	{
	public:
		Call(UE::UObject* a_obj, const wchar_t* a_fn);
		explicit operator bool() const { return m_fn != nullptr; }
		void* At(std::string_view a_name);
		template <class T>
		bool Set(std::string_view a_name, const T& a_value)
		{
			if (void* p = At(a_name)) {
				std::memcpy(p, &a_value, sizeof(T));
				return true;
			}
			return false;
		}
		template <class T>
		T Get(std::string_view a_name)
		{
			T v{};
			if (void* p = At(a_name)) std::memcpy(&v, p, sizeof(T));
			return v;
		}
		bool Run();

	private:
		UE::UObject*              m_obj;
		UE::UFunction*            m_fn;
		std::vector<std::uint8_t> m_params;
	};

	bool CallFirst(UE::UObject* a_obj, const wchar_t* a_fn, const void* a_bytes, std::size_t a_size);
	void Vec2(UE::UObject* a_obj, const wchar_t* a_fn, double a_x, double a_y);
	void Float(UE::UObject* a_obj, const wchar_t* a_fn, float a_v);
	void Visible(UE::UObject* a_widget, bool a_visible);   // HitTestInvisible / Collapsed
	void Colour(UE::UObject* a_image, float a_r, float a_g, float a_b, float a_a);

	UE::UClass*  Class(const wchar_t* a_path);
	UE::UObject* PlayerController();                         // a slot-checked handle, looked for at most every 2 s
	UE::UObject* CreateWidget(const wchar_t* a_classPath);   // WidgetBlueprintLibrary::Create, owned by the player controller

	// a UserWidget with a CanvasPanel root: returns the canvas, or nullptr
	UE::UObject* RootCanvas(UE::UObject* a_userWidget, const wchar_t* a_name);
	// a new widget of a_class inside a canvas; *a_slot gets its CanvasPanelSlot
	UE::UObject* AddToCanvas(UE::UObject* a_canvas, const wchar_t* a_class, const wchar_t* a_name, UE::UObject** a_slot);
	void         Anchors(UE::UObject* a_slot, double a_minX, double a_minY, double a_maxX, double a_maxY);

	// the brush drawn as a rounded box (a_circle: half-height radius) with a fill and an outline colour
	bool RoundedBox(UE::UObject* a_image, bool a_circle, const float a_fill[4], const float a_outline[4], float a_width);
}
