#pragma once

#include "npc/equipment_catalog.hpp"

#include <entitymodule/C_EquipmentManager.h>
#include <entitymodule/C_Item.h>
#include <entitymodule/S_ItemClass.h>
#include <framework/GuidUtils.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace kcd2o::kcse
{
	inline constexpr std::array<std::string_view, 8>
	    native_weapon_equipment_slots{
	        "PrimaryMainHand",
	        "PrimaryOffHand",
	        "SecondaryMainHand",
	        "SecondaryOffHand",
	        "Oversized",
	        "OversizedOff",
	        "Torch",
	        "Dagger"};

	[[nodiscard]] inline std::optional<std::string> native_equipped_slot(
	    const wh::entitymodule::C_EquipmentManager &equipment,
	    const wh::entitymodule::C_Item &item)
	{
		for (std::size_t index = 0; index < native_weapon_equipment_slots.size(); ++index)
		{
			if (equipment.m_weaponEquipSlots[index] == &item)
			{
				return std::string(native_weapon_equipment_slots[index]);
			}
		}

		for (const auto &[native_slot, item_wuid] : equipment.m_clothing)
		{
			if (item_wuid != item.m_wuid)
			{
				continue;
			}
			if (const auto *slot = npc::runtime_equipment_catalog().find_slot(native_slot))
			{
				return slot->name;
			}
		}
		return std::nullopt;
	}

	[[nodiscard]] inline std::optional<std::string> resolved_equipped_slot(
	    const wh::entitymodule::C_EquipmentManager &equipment,
	    const wh::entitymodule::C_Item &item)
	{
		if (auto native = native_equipped_slot(equipment, item))
		{
			return native;
		}
		if (!item.m_pClassData)
		{
			return std::nullopt;
		}
		const auto *definition = npc::runtime_equipment_catalog().find(
		    wh::FormatGuid(item.m_pClassData->m_guid));
		return definition
		    ? std::optional<std::string>(definition->equipped_slot)
		    : std::nullopt;
	}
}
