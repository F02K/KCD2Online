#pragma once

#include <cstdint>
#include <string_view>

namespace wh::entitymodule
{
	class C_Inventory;
	class C_Item;
}

namespace kcd2o::kcse
{
	// Shared native-inventory primitives. Higher-level profile and remote-avatar
	// transactions build on this module instead of duplicating item lookup.
	inline constexpr std::uint32_t native_item_equipped = 1U;

	[[nodiscard]] wh::entitymodule::C_Item *find_inventory_item(
	    wh::entitymodule::C_Inventory &inventory,
	    std::string_view instance_id);
}
