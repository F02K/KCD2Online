#pragma once

#include <cstdint>

namespace kcd2o::kcse
{
	struct player_respawn_identity
	{
		std::uintptr_t entity{};
		std::uintptr_t actor{};
		std::uintptr_t soul{};
		std::uint32_t entity_id{};
		std::uint64_t soul_guid_high{};
		std::uint64_t soul_guid_low{};
		std::uint64_t shared_soul_guid_high{};
		std::uint64_t shared_soul_guid_low{};

		[[nodiscard]] constexpr bool complete() const noexcept
		{
			return entity != 0 && actor != 0 && soul != 0 && entity_id != 0;
		}
	};

	enum class player_respawn_identity_result
	{
		stable,
		incomplete_before,
		incomplete_after,
		entity_changed,
		actor_changed,
		soul_changed,
		soul_guid_changed,
		shared_soul_guid_changed
	};

	[[nodiscard]] constexpr player_respawn_identity_result
	validate_player_respawn_identity(
	    const player_respawn_identity &before,
	    const player_respawn_identity &after) noexcept
	{
		if (!before.complete())
			return player_respawn_identity_result::incomplete_before;
		if (!after.complete())
			return player_respawn_identity_result::incomplete_after;
		if (before.entity != after.entity
		    || before.entity_id != after.entity_id)
		{
			return player_respawn_identity_result::entity_changed;
		}
		if (before.actor != after.actor)
			return player_respawn_identity_result::actor_changed;
		if (before.soul != after.soul)
			return player_respawn_identity_result::soul_changed;
		if (before.soul_guid_high != after.soul_guid_high
		    || before.soul_guid_low != after.soul_guid_low)
		{
			return player_respawn_identity_result::soul_guid_changed;
		}
		if (before.shared_soul_guid_high != after.shared_soul_guid_high
		    || before.shared_soul_guid_low != after.shared_soul_guid_low)
		{
			return player_respawn_identity_result::shared_soul_guid_changed;
		}
		return player_respawn_identity_result::stable;
	}

	[[nodiscard]] constexpr const char *to_string(
	    player_respawn_identity_result result) noexcept
	{
		switch (result)
		{
		case player_respawn_identity_result::stable: return "stable";
		case player_respawn_identity_result::incomplete_before:
			return "incomplete before revive";
		case player_respawn_identity_result::incomplete_after:
			return "incomplete after revive";
		case player_respawn_identity_result::entity_changed:
			return "client entity changed";
		case player_respawn_identity_result::actor_changed:
			return "player actor changed";
		case player_respawn_identity_result::soul_changed:
			return "player soul changed";
		case player_respawn_identity_result::soul_guid_changed:
			return "player soul GUID changed";
		case player_respawn_identity_result::shared_soul_guid_changed:
			return "player shared-Soul GUID changed";
		}
		return "unknown player identity result";
	}
}
