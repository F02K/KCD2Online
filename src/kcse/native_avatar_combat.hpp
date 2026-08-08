#pragma once

#include "kcse/native_weapon_controller.hpp"
#include "multiplayer/protocol.hpp"

#include <optional>

namespace kcd2o::npc
{
	enum class weapon_class;
}

namespace wh::entitymodule
{
	class C_Human;
}

namespace kcd2o::kcse
{
	// Multiplayer/native translation boundary for avatar combat visuals.
	// Capture and remote application live here; native primitives stay in the
	// weapon controller and combat observer modules.
	[[nodiscard]] protocol::AvatarWeaponClass protocol_weapon_class(
	    npc::weapon_class value) noexcept;
	[[nodiscard]] protocol::AvatarWeaponSet protocol_weapon_set(
	    native_weapon_set set) noexcept;
	[[nodiscard]] std::optional<native_weapon_set> native_weapon_set_for(
	    protocol::AvatarWeaponSet set) noexcept;

	void capture_native_avatar_combat(
	    protocol::AvatarDescriptor &avatar,
	    const wh::entitymodule::C_Human &human);
	[[nodiscard]] bool native_avatar_weapon_state_matches(
	    const wh::entitymodule::C_Human &human,
	    const protocol::AvatarDescriptor &avatar) noexcept;
	[[nodiscard]] bool apply_native_avatar_weapon_state(
	    wh::entitymodule::C_Human &human,
	    const protocol::AvatarDescriptor &avatar) noexcept;
}
