#pragma once

#include <cstdint>
#include <optional>

namespace wh::entitymodule
{
	class C_Human;
}

namespace kcd2o::kcse
{
	// Low-level Human weapon-controller boundary. This module knows native
	// controller slots and REL calls, but nothing about multiplayer messages.
	enum class native_weapon_set : std::uint32_t
	{
		primary = 0,
		secondary = 1,
		oversized = 2,
		any = 3
	};

	[[nodiscard]] bool is_weapon_set_drawn(
	    const wh::entitymodule::C_Human &human,
	    native_weapon_set set) noexcept;
	[[nodiscard]] std::optional<native_weapon_set> drawn_weapon_set(
	    const wh::entitymodule::C_Human &human) noexcept;
	[[nodiscard]] bool set_weapon_set_drawn(
	    wh::entitymodule::C_Human &human,
	    native_weapon_set set,
	    bool drawn) noexcept;
}
