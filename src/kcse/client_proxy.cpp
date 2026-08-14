#include "kcse/client_proxy.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string_view>
#include <utility>
#include <Windows.h>

namespace kcd2o::kcse
{
	namespace
	{
		std::atomic<const client_api *> g_api{};

		template<std::size_t N>
		void copy_text(char (&target)[N], std::string_view value)
		{
			const auto count = std::min(value.size(), N - 1);
			std::memcpy(target, value.data(), count);
			target[count] = '\0';
		}

	} // namespace

	const client_api *ui_client_proxy::load() const noexcept
	{
		if (const auto *loaded = g_api.load(std::memory_order_acquire))
		{
			return loaded;
		}
		const auto module = GetModuleHandleW(client_module_name);
		if (!module)
		{
			return nullptr;
		}
		const auto query      = reinterpret_cast<query_client>(GetProcAddress(module, client_query_export));
		const auto *candidate = query ? query(kcd2o_version_major, kcd2o_version_minor, kcd2o_version_patch) : nullptr;
		if (!compatible(candidate))
		{
			return nullptr;
		}
		g_api.store(candidate, std::memory_order_release);
		return candidate;
	}

	bool ui_client_proxy::available() const noexcept
	{
		return load() != nullptr;
	}

	runtime_gate ui_client_proxy::runtime_capability() const
	{
		runtime_status value;
		const auto *api = load();
		if (!api || api->get_runtime_status(&value) == 0)
		{
			return {false, false, "KCD2Online KCSE client is not loaded."};
		}
		return {value.available != 0, false, value.diagnostic};
	}

	bool ui_client_proxy::can_start_join() const
	{
		runtime_status value;
		const auto *api = load();
		return api && api->get_runtime_status(&value) != 0 && value.joinable != 0;
	}

	std::string ui_client_proxy::current_level_id() const
	{
		runtime_status value;
		const auto *api = load();
		return api && api->get_runtime_status(&value) != 0 ? std::string(value.level_id) : std::string{};
	}

	bool ui_client_proxy::connect(const client_options &options) const
	{
		const auto *api = load();
		if (!api)
		{
			return false;
		}
		connect_request request;
		copy_text(request.address, options.address);
		copy_text(request.display_name, options.display_name);
		copy_text(request.password, options.password);
		copy_text(request.content_hash, options.content_hash);
		copy_text(request.claim_code, options.claim_code);
		copy_text(request.server_id, options.server_id);
		copy_text(request.account_service_url, options.account_service_url);
		return api->connect(&request) != 0;
	}

	void ui_client_proxy::disconnect() const
	{
		if (const auto *api = load())
		{
			api->disconnect();
		}
	}

	bool ui_client_proxy::send_chat(std::string text) const
	{
		const auto *api = load();
		return api && api->send_chat(text.c_str()) != 0;
	}

	bool ui_client_proxy::play_emote(emote_kind kind) const
	{
		const auto *api = load();
		return api && api->play_emote(static_cast<std::uint32_t>(kind)) != 0;
	}

	bool ui_client_proxy::select_avatar(std::string archetype_id) const
	{
		const auto *api = load();
		return api && api->select_avatar(archetype_id.c_str()) != 0;
	}

	bool ui_client_proxy::attempt_sleep() const
	{
		const auto *api = load();
		return api && api->attempt_sleep() != 0;
	}

	bool ui_client_proxy::request_respawn() const
	{
		const auto *api = load();
		return api && api->request_respawn() != 0;
	}

	bool ui_client_proxy::set_player_voice_volume(player_id player, float volume) const
	{
		const auto *api = load();
		return api && api->set_player_voice_volume(player, volume) != 0;
	}

	void ui_client_proxy::set_diagnostic_logging(bool enabled) const
	{
		if (const auto *api = load())
		{
			api->set_diagnostic_logging(enabled ? 1U : 0U);
		}
	}

	client_status ui_client_proxy::status() const
	{
		client_status_view value;
		const auto *api = load();
		if (!api || api->get_status(&value) == 0)
		{
			client_status result;
			result.error = "KCD2Online KCSE client is not loaded.";
			return result;
		}
		client_status result;
		const auto state = static_cast<client_state>(value.state);
		if (!is_valid_client_state(state))
		{
			result.error = "KCD2Online KCSE client returned an invalid state value.";
			return result;
		}
		result.state                     = state;
		result.local_player_id           = value.local_player_id;
		result.ping_ms                   = value.ping_ms;
		result.packet_loss_percent       = value.packet_loss_percent;
		result.game_queue_size           = value.game_queue_size;
		result.server_name               = value.server_name;
		result.server_id                 = value.server_id;
		result.session_id                = value.session_id;
		result.level_id                  = value.level_id;
		result.error                     = value.error;
		result.avatar_archetype_id       = value.avatar_archetype_id;
		result.sleeping                  = value.sleeping != 0;
		result.sleeping_players          = value.sleeping_players;
		result.sleeping_players_required = value.sleeping_players_required;
		result.dead                      = value.dead != 0;
		result.respawn_pending           = value.respawn_pending != 0;
		result.voice_recording           = value.voice_recording != 0;
		result.voice_speaking            = value.voice_speaking != 0;
		result.voice_level               = std::clamp(value.voice_level, 0.0F, 1.0F);
		const auto voice_range           = static_cast<protocol::VoiceRange>(value.voice_range);
		if (voice_range >= protocol::VOICE_RANGE_NORMAL && voice_range <= protocol::VOICE_RANGE_SHOUT)
		{
			result.voice_range = voice_range;
		}
		result.native_keybinds              = value.native_keybinds != 0;
		result.chat_action_generation       = value.chat_action_generation;
		result.emote_action_held            = value.emote_action_held != 0;
		result.staff_action_generation      = value.staff_action_generation;
		result.player_hub_action_generation = value.player_hub_action_generation;
		result.social_action_generation     = value.social_action_generation;
		result.environment_available        = value.environment_available != 0;
		result.time_of_day_hours            = value.time_of_day_hours;
		result.time_scale                   = value.time_scale;
		result.weather_id                   = value.weather_id;
		if (value.network_role <= static_cast<std::uint32_t>(protocol::NETWORK_ROLE_OWNER))
		{
			result.network_role = static_cast<protocol::NetworkRole>(value.network_role);
		}
		std::string_view permissions(value.effective_permissions);
		while (!permissions.empty())
		{
			const auto separator  = permissions.find('\n');
			const auto permission = permissions.substr(0, separator);
			if (!permission.empty())
			{
				result.effective_permissions.emplace_back(permission);
			}
			if (separator == std::string_view::npos)
			{
				break;
			}
			permissions.remove_prefix(separator + 1);
		}
		result.error_code                     = value.error_code;
		result.restriction_scope              = value.restriction_scope;
		result.restriction_kind               = value.restriction_kind;
		result.restriction_reason             = value.restriction_reason;
		result.restriction_expires_at_unix_ms = value.restriction_expires_at_unix_ms;
		result.restriction_reference_id       = value.restriction_reference_id;
		result.support_url                    = value.support_url;
		result.avatar_policy.set_default_archetype_id(value.default_avatar_archetype_id);
		const auto count = api->copy_avatar_archetypes(nullptr, 0);
		std::vector<fixed_string> archetypes(count);
		if (count != 0)
		{
			archetypes.resize(std::min(count, api->copy_avatar_archetypes(archetypes.data(), count)));
		}
		for (const auto &archetype : archetypes)
		{
			result.avatar_policy.add_allowed_archetype_ids(archetype.value);
		}
		return result;
	}

	std::vector<kcd2o::remote_player_view> ui_client_proxy::players() const
	{
		const auto *api = load();
		if (!api)
		{
			return {};
		}
		const auto count = api->copy_players(nullptr, 0);
		std::vector<kcd2o::kcse::remote_player_view> raw(count);
		if (count != 0)
		{
			raw.resize(std::min(count, api->copy_players(raw.data(), count)));
		}
		std::vector<kcd2o::remote_player_view> result;
		result.reserve(raw.size());
		for (const auto &value : raw)
		{
			kcd2o::remote_player_view player;
			player.id            = value.player_id;
			player.connected     = value.connected != 0;
			player.movement_mode = static_cast<protocol::MovementMode>(value.movement_mode);
			player.display_name  = value.display_name;
			player.persistent_id = value.persistent_id;
			if (value.network_role <= static_cast<std::uint32_t>(protocol::NETWORK_ROLE_OWNER))
			{
				player.network_role = static_cast<protocol::NetworkRole>(value.network_role);
			}
			result.push_back(std::move(player));
		}
		return result;
	}

	std::vector<chat_entry> ui_client_proxy::chat_history() const
	{
		const auto *api = load();
		if (!api)
		{
			return {};
		}
		const auto count = api->copy_chat(nullptr, 0);
		std::vector<chat_entry_view> raw(count);
		if (count != 0)
		{
			raw.resize(std::min(count, api->copy_chat(raw.data(), count)));
		}
		std::vector<chat_entry> result;
		result.reserve(raw.size());
		for (const auto &value : raw)
		{
			result.push_back({value.player_id,
			                  value.display_name,
			                  value.text,
			                  value.server_time_ms,
			                  static_cast<protocol::ChatChannel>(value.channel),
			                  value.network_role <= static_cast<std::uint32_t>(protocol::NETWORK_ROLE_OWNER) ?
			                      static_cast<protocol::NetworkRole>(value.network_role) :
			                      protocol::NETWORK_ROLE_USER});
		}
		return result;
	}

	ui_client_proxy &ui_client()
	{
		static ui_client_proxy instance;
		return instance;
	}
} // namespace kcd2o::kcse
