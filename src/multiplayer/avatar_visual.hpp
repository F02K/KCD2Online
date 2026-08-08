#pragma once

#include "multiplayer/protocol.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kcd2o
{
	[[nodiscard]] inline bool avatar_weapon_should_draw(
	    const protocol::AvatarDescriptor &avatar) noexcept
	{
		return avatar.weapon_drawn()
		    && avatar.weapon_class()
		        != protocol::AVATAR_WEAPON_CLASS_NONE;
	}

	inline void canonicalize_avatar_visual(protocol::AvatarDescriptor &avatar)
	{
		if (!avatar.weapon_drawn())
		{
			avatar.set_active_weapon_set(protocol::AVATAR_WEAPON_SET_NONE);
			avatar.set_weapon_class(protocol::AVATAR_WEAPON_CLASS_NONE);
			avatar.set_stance(protocol::AVATAR_STANCE_RELAXED);
		}
		else
		{
			avatar.set_stance(protocol::AVATAR_STANCE_READY);
			// Compatibility for descriptors written before active_weapon_set was
			// introduced. New captures always provide the exact native set.
			if (avatar.active_weapon_set() == protocol::AVATAR_WEAPON_SET_NONE)
			{
				switch (avatar.weapon_class())
				{
				case protocol::AVATAR_WEAPON_CLASS_BOW:
				case protocol::AVATAR_WEAPON_CLASS_CROSSBOW:
					avatar.set_active_weapon_set(protocol::AVATAR_WEAPON_SET_SECONDARY);
					break;
				case protocol::AVATAR_WEAPON_CLASS_POLEARM:
					avatar.set_active_weapon_set(protocol::AVATAR_WEAPON_SET_OVERSIZED);
					break;
				case protocol::AVATAR_WEAPON_CLASS_NONE: break;
				default:                                 avatar.set_active_weapon_set(protocol::AVATAR_WEAPON_SET_PRIMARY); break;
				}
			}
		}

		std::vector<protocol::AvatarEquipment> equipment{avatar.equipment().begin(), avatar.equipment().end()};
		std::ranges::sort(equipment,
		                  {},
		                  [](const protocol::AvatarEquipment &item)
		                  {
			                  return std::pair{item.equipped_slot(), item.definition_id()};
		                  });
		if (equipment.size() > max_avatar_equipment_items)
		{
			equipment.resize(max_avatar_equipment_items);
		}
		avatar.clear_equipment();
		for (auto &item : equipment)
		{
			*avatar.add_equipment() = std::move(item);
		}
	}

	inline void replace_avatar_equipment_from_profile(
	    protocol::AvatarDescriptor &avatar,
	    const protocol::PlayerProfile &profile)
	{
		avatar.clear_equipment();
		avatar.set_weapon_class(protocol::AVATAR_WEAPON_CLASS_NONE);
		avatar.set_active_weapon_set(protocol::AVATAR_WEAPON_SET_NONE);
		for (const auto &item : profile.inventory())
		{
			if (!item.has_equipped_slot())
				continue;
			auto *visible = avatar.add_equipment();
			visible->set_definition_id(item.definition_id());
			visible->set_equipped_slot(item.equipped_slot());
		}
	}

	[[nodiscard]] inline bool same_avatar_equipment(
	    const protocol::AvatarDescriptor &left,
	    const protocol::AvatarDescriptor &right)
	{
		if (left.archetype_id() != right.archetype_id()
		    || left.equipment_size() != right.equipment_size())
		{
			return false;
		}
		for (int index = 0; index < left.equipment_size(); ++index)
		{
			if (left.equipment(index).definition_id()
			        != right.equipment(index).definition_id()
			    || left.equipment(index).equipped_slot()
			        != right.equipment(index).equipped_slot())
			{
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] inline bool same_avatar_visual(const protocol::AvatarDescriptor &left, const protocol::AvatarDescriptor &right)
	{
		return same_avatar_equipment(left, right)
		    && left.stance() == right.stance()
		    && left.weapon_class() == right.weapon_class()
		    && left.weapon_drawn() == right.weapon_drawn()
		    && left.active_weapon_set() == right.active_weapon_set()
		    && left.combat_mode() == right.combat_mode()
		    && left.active_in_combat() == right.active_in_combat();
	}

	[[nodiscard]] inline protocol::AvatarDescriptor merge_avatar_visual(const protocol::AvatarDescriptor &authoritative, const std::optional<protocol::AvatarDescriptor> &local_visual, const std::optional<std::string> &selected_archetype)
	{
		auto result = authoritative;
		if (selected_archetype)
		{
			result.set_archetype_id(*selected_archetype);
		}
		if (local_visual)
		{
			result.clear_equipment();
			for (const auto &item : local_visual->equipment())
			{
				*result.add_equipment() = item;
			}
			result.set_stance(local_visual->stance());
			result.set_weapon_class(local_visual->weapon_class());
			result.set_weapon_drawn(local_visual->weapon_drawn());
			result.set_active_weapon_set(local_visual->active_weapon_set());
			result.set_combat_mode(local_visual->combat_mode());
			result.set_active_in_combat(local_visual->active_in_combat());
		}
		result.set_revision(authoritative.revision());
		canonicalize_avatar_visual(result);
		return result;
	}
} // namespace kcd2o
