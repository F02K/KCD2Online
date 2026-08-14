#pragma once

#include <cstdint>

namespace big::ingame_player_hub
{
	// Draws the player-facing multiplayer overview independently of the mod menu.
	void render(bool another_panel_open);

	// F2 toggles the panel; Escape closes it.
	void on_window_message(std::uint32_t message, std::uintptr_t wparam) noexcept;

	[[nodiscard]] bool blocks_game_input() noexcept;
} // namespace big::ingame_player_hub
