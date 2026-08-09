#pragma once

#include <string>

namespace kcd2o
{
	struct multiplayer_ui_settings
	{
		std::string address;
		std::string display_name;
		std::string account_service_url;

		void persist_address() const;
		void persist_display_name() const;
	};

	[[nodiscard]] multiplayer_ui_settings &ui_settings();
}
