#pragma once

#include <cstdint>

namespace big::server_ui
{
	// Renders declarative UI documents delivered by the connected server.
	void render(bool mod_gui_open);
	void on_window_message(
	    std::uint32_t message,
	    std::uintptr_t wparam,
	    std::intptr_t lparam) noexcept;
	[[nodiscard]] bool blocks_game_input() noexcept;
}
