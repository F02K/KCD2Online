#pragma once

#include <cstdint>

namespace big::ingame_player_hub
{
	// Draws the player-facing multiplayer overview independently of the mod menu.
	void render(bool another_panel_open);
	void toggle_from_resource() noexcept;

	// Resource Lua owns the open key; Escape remains a universal close action.
	void on_window_message(std::uint32_t message, std::uintptr_t wparam) noexcept;

	[[nodiscard]] bool blocks_game_input() noexcept;
} // namespace big::ingame_player_hub
