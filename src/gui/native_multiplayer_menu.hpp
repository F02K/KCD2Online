#pragma once

#include <Windows.h>

namespace big::native_multiplayer_menu
{
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
	void before_show_page(void *menu) noexcept;
	void update() noexcept;
	[[nodiscard]] bool on_window_message(
	    UINT message,
	    WPARAM wparam,
	    LPARAM lparam) noexcept;
	[[nodiscard]] bool blocks_game_input() noexcept;
#else
	inline void before_show_page(void *) noexcept
	{
	}

	inline void update() noexcept
	{
	}

	[[nodiscard]] inline bool on_window_message(
	    UINT,
	    WPARAM,
	    LPARAM) noexcept
	{
		return false;
	}

	[[nodiscard]] inline bool blocks_game_input() noexcept
	{
		return false;
	}
#endif
}
