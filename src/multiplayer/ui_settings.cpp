#include "multiplayer/ui_settings.hpp"

#include <config/config.hpp>

namespace kcd2o
{
	namespace
	{
		auto *address_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer",
			    "Address",
			    std::string{"127.0.0.1:27020"},
			    "Last Direct-IP server address.");
			return entry;
		}

		auto *display_name_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer",
			    "Display Name",
			    std::string{"Henry"},
			    "Last multiplayer display name.");
			return entry;
		}
	}

	void multiplayer_ui_settings::persist_address() const
	{
		address_entry()->set_value(address);
	}

	void multiplayer_ui_settings::persist_display_name() const
	{
		display_name_entry()->set_value(display_name);
	}

	multiplayer_ui_settings &ui_settings()
	{
		static multiplayer_ui_settings settings{
		    address_entry()->get_value(),
		    display_name_entry()->get_value()};
		return settings;
	}
}
