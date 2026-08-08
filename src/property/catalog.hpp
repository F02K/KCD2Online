#pragma once

#include "multiplayer/protocol.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace kcd2o::property
{
	inline constexpr std::uint32_t catalog_schema = 1;

	[[nodiscard]] bool scan_level_pak(
	    const std::filesystem::path &level_pak,
	    std::string_view level_id,
	    protocol::PropertyCatalog &output,
	    std::string &error);

	[[nodiscard]] std::filesystem::path level_pak_path(
	    const std::filesystem::path &game_root,
	    std::string_view level_id);
}
