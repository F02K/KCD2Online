#pragma once

#include "kcse/client_api.hpp"
#include "multiplayer/client.hpp"
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
		[[nodiscard]] bool select_avatar(std::string archetype_id) const;
		[[nodiscard]] bool attempt_sleep() const;
		[[nodiscard]] bool request_respawn() const;
		void set_diagnostic_logging(bool enabled) const;
		[[nodiscard]] client_status status() const;
		[[nodiscard]] std::vector<kcd2o::remote_player_view>
		remote_players() const;
		[[nodiscard]] std::vector<chat_entry> chat_history() const;

	private:
		[[nodiscard]] const client_api *load() const noexcept;
	};

	[[nodiscard]] ui_client_proxy &ui_client();
}
