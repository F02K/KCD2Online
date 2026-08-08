#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace kcd2o
{
	struct native_world_level
	{
		std::string_view id;
		std::string_view name;
		bool production;
	};

	// Mirrors Libs/Tables/level.xml from the supported KCD2 build. Production
	// servers should use 2/3/4. The remaining registered levels are retained for
	// development servers and explicit content-testing only.
	inline constexpr std::array native_world_levels{
	    native_world_level{"0", "rataje", false},
	    native_world_level{"1", "rataje_dlc4", false},
	    native_world_level{"2", "trosecko", true},
	    native_world_level{"3", "kutnohorsko", true},
	    native_world_level{"4", "klaster", true},
	    native_world_level{"256", "test_switching256", false},
	    native_world_level{"257", "test_switching257", false},
	    native_world_level{"258", "concept_level_switch_1", false},
	    native_world_level{"259", "concept_level_switch_2", false},
	    native_world_level{"300", "empty", false},
	    native_world_level{"400", "test_save", false},
	    native_world_level{"401", "test_switch_first", false},
	    native_world_level{"402", "test_switch_second", false},
	    native_world_level{"500", "player_switching", false},
	    native_world_level{"501", "player_switching2", false},
	};

	[[nodiscard]] constexpr std::optional<native_world_level>
	find_native_world_level(std::string_view value) noexcept
	{
		for (const auto &level : native_world_levels)
		{
			if (level.id == value || level.name == value)
				return level;
		}
		return std::nullopt;
	}

	[[nodiscard]] constexpr std::string_view canonical_level_id(
	    std::string_view value) noexcept
	{
		if (const auto level = find_native_world_level(value))
			return level->id;
		return value;
	}
}
