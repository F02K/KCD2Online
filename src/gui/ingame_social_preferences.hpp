#pragma once

#include "multiplayer/protocol.hpp"

#include <string_view>

namespace big::ingame_social_preferences
{
	// Local-only preferences are scoped to one server session because numeric
	// player IDs are not stable between sessions.
	void set_session(std::string_view session_id);

	[[nodiscard]] bool chat_hidden(kcd2o::player_id player);
	void set_chat_hidden(kcd2o::player_id player, bool hidden);

	[[nodiscard]] float voice_volume(kcd2o::player_id player);
	void set_voice_volume(kcd2o::player_id player, float volume);
} // namespace big::ingame_social_preferences
