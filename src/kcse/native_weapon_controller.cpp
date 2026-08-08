#include "kcse/native_weapon_controller.hpp"

#include <entitymodule/C_Human.h>
#include <REL/Relocation.h>

#include <array>

namespace kcd2o::kcse
{
	namespace
	{
		void *weapon_controller(const wh::entitymodule::C_Human &human)
		{
			auto **vtable = *reinterpret_cast<void ***>(
			    const_cast<wh::entitymodule::C_Human *>(&human));
			using function = void *(__fastcall *)(wh::entitymodule::C_Human *);
			return reinterpret_cast<function>(vtable[191])(
			    const_cast<wh::entitymodule::C_Human *>(&human));
		}
	}

	bool is_weapon_set_drawn(
	    const wh::entitymodule::C_Human &human,
	    native_weapon_set set) noexcept
	{
		auto *controller = weapon_controller(human);
		if (!controller)
			return false;
		using function = bool (__fastcall *)(
		    void *, bool, std::uint32_t, std::uint32_t);
		static REL::Relocation<function> query{REL::ID(47899)};
		return query(
		    controller,
		    true,
		    static_cast<std::uint32_t>(set),
		    27U);
	}

	std::optional<native_weapon_set> drawn_weapon_set(
	    const wh::entitymodule::C_Human &human) noexcept
	{
		constexpr std::array sets{
		    native_weapon_set::primary,
		    native_weapon_set::secondary,
		    native_weapon_set::oversized};
		for (const auto set : sets)
		{
			if (is_weapon_set_drawn(human, set))
				return set;
		}
		return std::nullopt;
	}

	bool set_weapon_set_drawn(
	    wh::entitymodule::C_Human &human,
	    native_weapon_set set,
	    bool drawn) noexcept
	{
		if (!drawn)
			return !human.IsWeaponDrawn() || human.SetWeaponDrawn(false);
		if (set == native_weapon_set::any)
			return human.SetWeaponDrawn(true);
		if (is_weapon_set_drawn(human, set))
			return true;

		auto *controller = weapon_controller(human);
		if (!controller)
			return false;
		using select_function = void (__fastcall *)(void *, std::uint32_t);
		static REL::Relocation<select_function> select{REL::ID(48939)};
		select(controller, static_cast<std::uint32_t>(set));
		using draw_function = void (__fastcall *)(void *);
		static REL::Relocation<draw_function> draw{REL::ID(351569)};
		draw(controller);
		return is_weapon_set_drawn(human, set);
	}
}
