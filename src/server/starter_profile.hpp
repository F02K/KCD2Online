#pragma once

#include "multiplayer/protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kcd2o::server
{
	struct starter_inventory_item
	{
		std::string definition_id;
		std::uint32_t count{1};
		float quality{1.0F};
		float condition{1.0F};
		std::optional<std::string> equipped_slot;
	};

	struct starter_profile_template
	{
		std::int64_t money{};
		std::vector<protocol::RpgValue> stats;
		std::vector<protocol::RpgValue> skills;
		std::vector<starter_inventory_item> inventory;
	};

	[[nodiscard]] starter_profile_template default_starter_profile_template();
	[[nodiscard]] starter_profile_template load_starter_profile_template(
	    const std::filesystem::path &path);
	void validate_starter_profile_template(
	    const starter_profile_template &profile);
	[[nodiscard]] protocol::PlayerProfile instantiate_starter_profile(
	    const starter_profile_template &profile,
	    player_id id,
	    std::string display_name,
	    std::string level_id);
}
