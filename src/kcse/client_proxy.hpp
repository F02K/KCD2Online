#pragma once

#include "kcse/client_api.hpp"
#include "multiplayer/client.hpp"
#include "multiplayer/emote_catalog.hpp"
#include "multiplayer/runtime.hpp"

#include <string>
#include <vector>

namespace kcd2o::kcse
{
	class ui_client_proxy
	{
	public:
		[[nodiscard]] bool available() const noexcept;
		[[nodiscard]] runtime_gate runtime_capability() const;
		[[nodiscard]] bool can_start_join() const;
		[[nodiscard]] std::string current_level_id() const;
		[[nodiscard]] bool connect(const client_options &options) const;
		void disconnect() const;
		[[nodiscard]] bool send_chat(std::string text) const;
		[[nodiscard]] bool play_emote(emote_kind kind) const;
		[[nodiscard]] bool select_avatar(std::string archetype_id) const;
		[[nodiscard]] bool attempt_sleep() const;
		[[nodiscard]] bool request_respawn() const;
		[[nodiscard]] bool set_player_voice_volume(player_id player, float volume) const;
		[[nodiscard]] voice_settings_view voice_settings() const;
		[[nodiscard]] bool set_voice_settings(const voice_settings_view &settings) const;
		[[nodiscard]] std::vector<voice_device_view> voice_devices() const;
		void refresh_voice_devices() const;
		void set_diagnostic_logging(bool enabled) const;
		[[nodiscard]] client_status status() const;
		[[nodiscard]] std::vector<kcd2o::remote_player_view> players() const;
		[[nodiscard]] std::vector<chat_entry> chat_history() const;
		[[nodiscard]] protocol::PropertyAccessSnapshot property_access() const;
		[[nodiscard]] bool request_property_role(
		    std::string property_id,
		    std::string target_player_id,
		    protocol::PropertyRole role,
		    std::uint64_t expires_at_ms = 0) const;
		[[nodiscard]] bool revoke_property_role(std::string assignment_id) const;
		[[nodiscard]] bool set_property_owner(
		    std::string property_id,
		    std::string target_player_id) const;
		[[nodiscard]] bool set_property_locked(
		    std::uint64_t entity_guid,
		    bool locked) const;
		[[nodiscard]] std::string resource_ui_json() const;
		[[nodiscard]] bool submit_resource_ui_event(
		    std::string resource,
		    std::string document,
		    std::string control,
		    std::string event,
		    std::string payload_json = "{}") const;

	private:
		[[nodiscard]] const client_api *load() const noexcept;
	};

	[[nodiscard]] ui_client_proxy &ui_client();
} // namespace kcd2o::kcse
