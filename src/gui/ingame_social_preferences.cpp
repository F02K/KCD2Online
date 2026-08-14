#include "gui/ingame_social_preferences.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace big::ingame_social_preferences
{
	namespace
	{
		struct preference_state
		{
			std::mutex mutex;
			std::string session_id;
			std::unordered_set<kcd2o::player_id> hidden_chat;
			std::unordered_map<kcd2o::player_id, float> voice_volumes;
		};

		preference_state &state()
		{
			static preference_state value;
			return value;
		}
	} // namespace

	void set_session(std::string_view session_id)
	{
		auto &value = state();
		std::scoped_lock lock(value.mutex);
		if (value.session_id == session_id)
		{
			return;
		}
		value.session_id = session_id;
		value.hidden_chat.clear();
		value.voice_volumes.clear();
	}

	bool chat_hidden(kcd2o::player_id player)
	{
		auto &value = state();
		std::scoped_lock lock(value.mutex);
		return value.hidden_chat.contains(player);
	}

	void set_chat_hidden(kcd2o::player_id player, bool hidden)
	{
		auto &value = state();
		std::scoped_lock lock(value.mutex);
		if (hidden)
		{
			value.hidden_chat.insert(player);
		}
		else
		{
			value.hidden_chat.erase(player);
		}
	}

	float voice_volume(kcd2o::player_id player)
	{
		auto &value = state();
		std::scoped_lock lock(value.mutex);
		const auto found = value.voice_volumes.find(player);
		return found == value.voice_volumes.end() ? 1.0F : found->second;
	}

	void set_voice_volume(kcd2o::player_id player, float volume)
	{
		auto &value = state();
		std::scoped_lock lock(value.mutex);
		volume = std::clamp(volume, 0.0F, 1.5F);
		if (volume == 1.0F)
		{
			value.voice_volumes.erase(player);
		}
		else
		{
			value.voice_volumes[player] = volume;
		}
	}
} // namespace big::ingame_social_preferences
