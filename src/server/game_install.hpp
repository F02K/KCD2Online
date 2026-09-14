#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kcd2o::server
{
	struct game_installation
	{
		std::filesystem::path root;
		std::filesystem::path executable;
		std::filesystem::path whgame;
		std::string source;
	};

	inline constexpr auto kcd2_steam_app_id = "1771300";
	inline constexpr auto kcd2_game_directory = "KingdomComeDeliverance2";
	inline constexpr auto kcd2_binary_directory =
	    "Bin/Win64MasterMasterSteamPGO";

	[[nodiscard]] std::optional<game_installation> inspect_game_root(
	    const std::filesystem::path &candidate,
	    std::string source = {});
	[[nodiscard]] std::vector<std::filesystem::path>
	default_steam_roots();
	[[nodiscard]] std::optional<game_installation>
	discover_steam_game_installation(
	    std::span<const std::filesystem::path> steam_roots);
}
