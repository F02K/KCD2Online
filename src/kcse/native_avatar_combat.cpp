#include "kcse/native_avatar_combat.hpp"

#include "kcse/native_combat_observer.hpp"
#include "multiplayer/avatar_visual.hpp"
#include "npc/equipment_catalog.hpp"

#include <entitymodule/C_Human.h>

#include <string_view>

namespace kcd2o::kcse
{
	namespace
	{
		bool slot_belongs_to_set(
		    std::string_view slot,
		    native_weapon_set set)
		{
			switch (set)
			{
			case native_weapon_set::primary:
				return slot == "PrimaryMainHand"
				    || slot == "PrimaryOffHand";
			case native_weapon_set::secondary:
				return slot == "SecondaryMainHand"
				    || slot == "SecondaryOffHand";
			case native_weapon_set::oversized:
				return slot == "Oversized" || slot == "OversizedOff";
			case native_weapon_set::any:
				return false;
			}
			return false;
		}

		protocol::AvatarWeaponClass weapon_class_for_set(
		    const protocol::AvatarDescriptor &avatar,
		    native_weapon_set set)
		{
			for (const auto &item : avatar.equipment())
			{
				if (!slot_belongs_to_set(item.equipped_slot(), set))
					continue;
				const auto *definition =
				    npc::runtime_equipment_catalog().find(
				        item.definition_id());
				if (!definition
				    || definition->weapon == npc::weapon_class::none)
				{
					continue;
				}
				return protocol_weapon_class(definition->weapon);
			}
			return set == native_weapon_set::primary
			    ? protocol::AVATAR_WEAPON_CLASS_UNARMED
			    : protocol::AVATAR_WEAPON_CLASS_NONE;
		}

	}

	protocol::AvatarWeaponClass protocol_weapon_class(
	    npc::weapon_class value) noexcept
	{
		switch (value)
		{
		case npc::weapon_class::unarmed:
			return protocol::AVATAR_WEAPON_CLASS_UNARMED;
		case npc::weapon_class::one_handed:
			return protocol::AVATAR_WEAPON_CLASS_ONE_HANDED;
		case npc::weapon_class::two_handed:
			return protocol::AVATAR_WEAPON_CLASS_TWO_HANDED;
		case npc::weapon_class::polearm:
			return protocol::AVATAR_WEAPON_CLASS_POLEARM;
		case npc::weapon_class::bow:
			return protocol::AVATAR_WEAPON_CLASS_BOW;
		case npc::weapon_class::crossbow:
			return protocol::AVATAR_WEAPON_CLASS_CROSSBOW;
		case npc::weapon_class::none:
			return protocol::AVATAR_WEAPON_CLASS_NONE;
		}
		return protocol::AVATAR_WEAPON_CLASS_NONE;
	}

	protocol::AvatarWeaponSet protocol_weapon_set(
	    native_weapon_set set) noexcept
	{
		switch (set)
		{
		case native_weapon_set::primary:
			return protocol::AVATAR_WEAPON_SET_PRIMARY;
		case native_weapon_set::secondary:
			return protocol::AVATAR_WEAPON_SET_SECONDARY;
		case native_weapon_set::oversized:
			return protocol::AVATAR_WEAPON_SET_OVERSIZED;
		case native_weapon_set::any:
			return protocol::AVATAR_WEAPON_SET_NONE;
		}
		return protocol::AVATAR_WEAPON_SET_NONE;
	}

	std::optional<native_weapon_set> native_weapon_set_for(
	    protocol::AvatarWeaponSet set) noexcept
	{
		switch (set)
		{
		case protocol::AVATAR_WEAPON_SET_PRIMARY:
			return native_weapon_set::primary;
		case protocol::AVATAR_WEAPON_SET_SECONDARY:
			return native_weapon_set::secondary;
		case protocol::AVATAR_WEAPON_SET_OVERSIZED:
			return native_weapon_set::oversized;
		case protocol::AVATAR_WEAPON_SET_NONE:
			return std::nullopt;
		}
		return std::nullopt;
	}

	void capture_native_avatar_combat(
	    protocol::AvatarDescriptor &avatar,
	    const wh::entitymodule::C_Human &human)
	{
		const auto active_set = drawn_weapon_set(human);
		if (active_set)
		{
			avatar.set_weapon_drawn(true);
			avatar.set_active_weapon_set(
			    protocol_weapon_set(*active_set));
			avatar.set_weapon_class(
			    weapon_class_for_set(avatar, *active_set));
		}
		else
		{
			avatar.set_weapon_drawn(false);
			avatar.set_active_weapon_set(
			    protocol::AVATAR_WEAPON_SET_NONE);
			avatar.set_weapon_class(
			    protocol::AVATAR_WEAPON_CLASS_NONE);
		}
		const auto combat = read_combat_state(human.m_pCombatActor);
		avatar.set_combat_mode(combat.combat_mode);
		avatar.set_active_in_combat(combat.active_in_combat);
		canonicalize_avatar_visual(avatar);
	}

	bool native_avatar_weapon_state_matches(
	    const wh::entitymodule::C_Human &human,
	    const protocol::AvatarDescriptor &avatar) noexcept
	{
		if (!avatar_weapon_should_draw(avatar))
			return !human.IsWeaponDrawn();
		const auto requested_set =
		    native_weapon_set_for(avatar.active_weapon_set());
		return requested_set
		    && drawn_weapon_set(human) == requested_set;
	}

	bool apply_native_avatar_weapon_state(
	    wh::entitymodule::C_Human &human,
	    const protocol::AvatarDescriptor &avatar) noexcept
	{
		if (!avatar_weapon_should_draw(avatar))
		{
			return set_weapon_set_drawn(
			    human,
			    native_weapon_set::any,
			    false);
		}
		const auto requested_set =
		    native_weapon_set_for(avatar.active_weapon_set());
		return requested_set
		    && set_weapon_set_drawn(human, *requested_set, true);
	}
}
