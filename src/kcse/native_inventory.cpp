#include "kcse/native_inventory.hpp"

#include <entitymodule/C_Inventory.h>
#include <entitymodule/C_Item.h>
#include <framework/GuidUtils.h>

#include <ranges>

namespace kcd2o::kcse
{
	wh::entitymodule::C_Item *find_inventory_item(
	    wh::entitymodule::C_Inventory &inventory,
	    std::string_view instance_id)
	{
		const auto found = std::ranges::find_if(
		    inventory.m_items,
		    [&](const auto *item)
		    {
			    return item
			        && wh::FormatGuid(item->m_instanceGuid) == instance_id;
		    });
		return found == inventory.m_items.end() ? nullptr : *found;
	}
}
