#include "Inventory.h"

#include "TesThread.h"

namespace inventory
{
	namespace
	{
		constexpr std::uint8_t kQuickKeyType = 0x55;
		constexpr std::ptrdiff_t kKeyIDOffset = 0x18;
		constexpr std::size_t kQuickKeySize = 0x20;

		RE::BSSimpleList<RE::ItemChange*>* Items()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return nullptr;
			}
			auto* changes = player->extra.GetExtraData<RE::ExtraContainerChanges>();
			if (!changes || !changes->changes) {
				return nullptr;
			}
			return changes->changes->list;
		}

		RE::ItemChange* Find(std::uint32_t a_formID)
		{
			auto* items = Items();
			if (!items || !a_formID) {
				return nullptr;
			}
			for (RE::ItemChange* item : *items) {
				if (item && item->object && item->object->GetFormID() == a_formID) {
					return item;
				}
			}
			return nullptr;
		}

		RE::BSExtraData* KeyExtra(RE::ExtraDataList* a_list)
		{
			for (RE::BSExtraData* x = a_list ? a_list->head : nullptr; x; x = x->next) {
				if (static_cast<std::uint8_t>(x->type.get()) == kQuickKeyType) {
					return x;
				}
			}
			return nullptr;
		}

		int KeyOf(const RE::BSExtraData* a_x)
		{
			return *(reinterpret_cast<const std::uint8_t*>(a_x) + kKeyIDOffset);
		}

		// A new ExtraQuickKey the way the game's own looks: vtable, type 0x55, next, keyID - on the game's heap.
		RE::BSExtraData* NewQuickKey(int a_key)
		{
			static REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE::ExtraQuickKey[0] };
			auto* mem = static_cast<std::uint8_t*>(RE::malloc(kQuickKeySize));
			if (!mem) {
				return nullptr;
			}
			std::memset(mem, 0, kQuickKeySize);
			*reinterpret_cast<std::uintptr_t*>(mem) = vtable.address();
			mem[0x08] = kQuickKeyType;
			mem[kKeyIDOffset] = static_cast<std::uint8_t>(a_key);
			return reinterpret_cast<RE::BSExtraData*>(mem);
		}

		// A new, empty ExtraDataList appended to the item's lists (nodes on the game's heap, never CRT new).
		RE::ExtraDataList* NewList(RE::ItemChange* a_item)
		{
			static REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE::ExtraDataList[0] };
			auto* list = static_cast<RE::ExtraDataList*>(RE::malloc(sizeof(RE::ExtraDataList)));
			if (!list) {
				return nullptr;
			}
			std::memset(static_cast<void*>(list), 0, sizeof(RE::ExtraDataList));
			*reinterpret_cast<std::uintptr_t*>(list) = vtable.address();

			using Node = RE::BSSimpleList<RE::ExtraDataList*>::Node;
			if (!a_item->extraData) {
				auto* head = static_cast<Node*>(RE::malloc(sizeof(Node)));   // the list object IS its first node
				if (!head) {
					return nullptr;
				}
				head->item = list;
				head->next = nullptr;
				a_item->extraData = reinterpret_cast<RE::BSSimpleList<RE::ExtraDataList*>*>(head);
				return list;
			}
			auto* node = reinterpret_cast<Node*>(a_item->extraData);
			if (!node->item) {
				node->item = list;   // an empty first node
				return list;
			}
			while (node->next) {
				node = node->next;
			}
			auto* added = static_cast<Node*>(RE::malloc(sizeof(Node)));
			if (!added) {
				return nullptr;
			}
			added->item = list;
			added->next = nullptr;
			node->next = added;
			return list;
		}
	}

	KeyMap Keys()
	{
		KeyMap out{};
		auto* items = Items();
		if (!items) {
			return out;
		}
		for (RE::ItemChange* item : *items) {
			if (!item || !item->object || !item->extraData) {
				continue;
			}
			for (RE::ExtraDataList* xl : *item->extraData) {
				if (auto* x = KeyExtra(xl)) {
					const int key = KeyOf(x);
					if (key >= 0 && key < kSlots) {
						out[key] = item->object->GetFormID();
					}
				}
			}
		}
		return out;
	}

	bool Has(std::uint32_t a_formID)
	{
		const auto* item = Find(a_formID);
		return item && item->count > 0;
	}

	std::string NameOf(std::uint32_t a_formID)
	{
		auto* form = RE::TESForm::LookupByID(a_formID);
		const char* n = form ? RE::TESFullName::GetFullName(form) : nullptr;
		return n && *n ? std::string(n) : std::format("0x{:08X}", a_formID);
	}

	std::uint32_t FindByName(const std::string& a_name)
	{
		auto* items = Items();
		if (!items || a_name.empty()) {
			return 0;
		}
		for (RE::ItemChange* item : *items) {
			if (!item || !item->object || item->count <= 0) {
				continue;
			}
			const char* n = RE::TESFullName::GetFullName(item->object);
			if (n && a_name == n) {
				return item->object->GetFormID();
			}
		}
		return 0;
	}

	void ClearKey(std::uint32_t a_formID)
	{
		auto* item = Find(a_formID);
		if (!item || !item->extraData) {
			return;
		}
		for (RE::ExtraDataList* xl : *item->extraData) {
			if (xl && KeyExtra(xl)) {
				xl->RemoveExtra(RE::EXTRA_DATA_TYPE(kQuickKeyType));
			}
		}
	}

	void ClearSlot(int a_key)
	{
		auto* items = Items();
		if (!items) {
			return;
		}
		for (RE::ItemChange* item : *items) {
			if (!item || !item->extraData) {
				continue;
			}
			for (RE::ExtraDataList* xl : *item->extraData) {
				if (auto* x = KeyExtra(xl); x && KeyOf(x) == a_key) {
					xl->RemoveExtra(RE::EXTRA_DATA_TYPE(kQuickKeyType));
				}
			}
		}
	}

	bool SetKey(std::uint32_t a_formID, int a_key)
	{
		if (a_key < 0 || a_key >= kSlots) {
			return false;
		}
		auto* item = Find(a_formID);
		if (!item || item->count <= 0) {
			logger::warn("inventory: {} is not carried - quick key {} left as it was", NameOf(a_formID), a_key + 1);
			return false;
		}
		ClearSlot(a_key);
		// already keyed (to another slot): move that key
		if (item->extraData) {
			for (RE::ExtraDataList* xl : *item->extraData) {
				if (auto* x = KeyExtra(xl)) {
					*(reinterpret_cast<std::uint8_t*>(x) + kKeyIDOffset) = static_cast<std::uint8_t>(a_key);
					return true;
				}
			}
		}
		// the item's first extra list, or a new one
		RE::ExtraDataList* target = nullptr;
		if (item->extraData) {
			for (RE::ExtraDataList* xl : *item->extraData) {
				if (xl) {
					target = xl;
					break;
				}
			}
		}
		if (!target) {
			target = NewList(item);
		}
		auto* key = target ? NewQuickKey(a_key) : nullptr;
		if (!key) {
			logger::error("inventory: no memory for quick key {} on {}", a_key + 1, NameOf(a_formID));
			return false;
		}
		target->AddExtra(key);
		return true;
	}

	std::string PlayerName()
	{
		// the name chosen at character creation lives on the player's base record; the reference itself reads "Player"
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* base = player ? player->data.objectReference : nullptr;
		const char* n = base ? RE::TESFullName::GetFullName(base) : nullptr;
		if (!n || !*n) {
			n = player ? RE::TESFullName::GetFullName(player) : nullptr;
		}
		return n && *n ? std::string(n) : std::string("Player");
	}

	State StateOf(std::uint32_t a_formID)
	{
		State out;
		auto* item = Find(a_formID);
		if (!item || item->count <= 0) {
			return out;
		}
		out.carried = true;
		out.count = item->count;
		if (item->extraData) {
			for (RE::ExtraDataList* xl : *item->extraData) {
				if (xl && (xl->GetExtraData(RE::EXTRA_DATA_TYPE::Worn) || xl->GetExtraData(RE::EXTRA_DATA_TYPE::WornLeft))) {
					out.worn = true;
				}
			}
		}
		return out;
	}

	std::string Describe(const State& a_state)
	{
		return a_state.carried ? std::format("x{}{}", a_state.count, a_state.worn ? ", worn" : ", not worn") : std::string("not carried");
	}

	void EquipKeyed(std::uint32_t a_formID)
	{
		testhread::Post([a_formID] {
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* item = Find(a_formID);
			if (!player || !item || !item->object || item->count <= 0) {
				logger::info("wheels: fallback equip - {} is not carried any more", NameOf(a_formID));
				return;
			}
			// the instance carrying the quick key (an enchanted or damaged one is its own list), else the base item
			RE::ExtraDataList* keyed = nullptr;
			if (item->extraData) {
				for (RE::ExtraDataList* xl : *item->extraData) {
					if (xl && KeyExtra(xl)) {
						keyed = xl;
						break;
					}
				}
			}
			// no lock: EquipObject's last argument is the console's NoUnequip (gate rule or-equipobject-never-locks)
			player->EquipObject(item->object, 1, keyed, false, false);
			logger::info("wheels: fallback equip - {} equipped on the TES thread ({})", NameOf(a_formID), Describe(StateOf(a_formID)));
		});
	}
}
