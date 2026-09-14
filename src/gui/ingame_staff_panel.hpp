#pragma once

#include <cstdint>

namespace big::ingame_staff_panel
{
	// Draws the permission-gated staff workspace independently of the mod menu.
	void render(bool mod_gui_open);

	// F7 toggles the panel; Escape closes it.
	void on_window_message(std::uint32_t message, std::uintptr_t wparam) noexcept;

	[[nodiscard]] bool blocks_game_input() noexcept;
} // namespace big::ingame_staff_panel
