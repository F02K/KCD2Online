#pragma once

#include <cstdint>

namespace big::ingame_property_panel
{
	void render(bool another_panel_open);
	void on_window_message(std::uint32_t message, std::uintptr_t wparam) noexcept;
	[[nodiscard]] bool blocks_game_input() noexcept;
}
