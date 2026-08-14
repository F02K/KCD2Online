#pragma once

#include <cstdint>

namespace kcd2o::kcse::native_keybinds
{
	struct input_state
	{
		bool available{};
		std::uint32_t chat_generation{};
		std::uint32_t player_hub_generation{};
		std::uint32_t social_generation{};
		std::uint32_t staff_generation{};
		bool emote_held{};
	};

	[[nodiscard]] bool install();
	[[nodiscard]] bool available() noexcept;
	[[nodiscard]] bool voice_held() noexcept;
	[[nodiscard]] input_state state() noexcept;
	void reset_transient() noexcept;
} // namespace kcd2o::kcse::native_keybinds
