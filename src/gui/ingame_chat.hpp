#pragma once

#include <cstdint>

namespace big::ingame_chat
{
	// Draws the lightweight multiplayer HUD independently of the mod menu.
	void render(bool mod_gui_open);

	// Window messages feed the open/cancel transitions. Text entry itself is
	// handled by ImGui's Win32 backend.
	void on_window_message(std::uint32_t message, std::uintptr_t wparam) noexcept;

	[[nodiscard]] bool blocks_game_input() noexcept;
	[[nodiscard]] bool allows_blocked_input(std::uint32_t input_state) noexcept;
} // namespace big::ingame_chat
