#pragma once

#include <cstdint>

namespace kcd2o
{
	enum class client_state : std::uint32_t
	{
		disconnected,
		preflight,
		authenticating,
		waiting_for_bootstrap,
		loading_sandbox,
		applying_profile,
		connected,
		reconnecting,
		closing,
		runtime_preflight,
		connecting
	};

	[[nodiscard]] constexpr bool is_valid_client_state(
	    client_state state) noexcept
	{
		switch (state)
		{
		case client_state::disconnected:
		case client_state::runtime_preflight:
		case client_state::connecting:
		case client_state::preflight:
		case client_state::authenticating:
		case client_state::waiting_for_bootstrap:
		case client_state::loading_sandbox:
		case client_state::applying_profile:
		case client_state::connected:
		case client_state::reconnecting:
		case client_state::closing:
			return true;
		}
		return false;
	}

	[[nodiscard]] constexpr bool is_valid_client_transition(
	    client_state from,
	    client_state to) noexcept
	{
		if (!is_valid_client_state(from) || !is_valid_client_state(to))
			return false;
		if (from == to)
			return from == client_state::disconnected;
		if (from == client_state::closing)
			return to == client_state::disconnected;
		if (from != client_state::disconnected
		    && (to == client_state::closing
		        || to == client_state::disconnected))
		{
			return true;
		}

		switch (from)
		{
		case client_state::disconnected:
			return to == client_state::runtime_preflight;
		case client_state::runtime_preflight:
			return to == client_state::connecting;
		case client_state::connecting:
			return to == client_state::preflight
			    || to == client_state::reconnecting;
		case client_state::preflight:
			return to == client_state::authenticating
			    || to == client_state::reconnecting;
		case client_state::authenticating:
			return to == client_state::waiting_for_bootstrap
			    || to == client_state::loading_sandbox
			    || to == client_state::reconnecting;
		case client_state::waiting_for_bootstrap:
			return to == client_state::loading_sandbox
			    || to == client_state::reconnecting;
		case client_state::loading_sandbox:
			return to == client_state::applying_profile
			    || to == client_state::reconnecting;
		case client_state::applying_profile:
			return to == client_state::connected
			    || to == client_state::reconnecting;
		case client_state::connected:
			return to == client_state::reconnecting;
		case client_state::reconnecting:
			return to == client_state::connecting;
		case client_state::closing:
			break;
		}
		return false;
	}

	[[nodiscard]] constexpr const char *to_string(client_state state) noexcept
	{
		switch (state)
		{
		case client_state::disconnected:
			return "Disconnected";
		case client_state::runtime_preflight:
			return "Runtime preflight";
		case client_state::connecting:
			return "Connecting";
		case client_state::preflight:
			return "Protocol preflight";
		case client_state::authenticating:
			return "Authenticating";
		case client_state::waiting_for_bootstrap:
			return "Waiting for bootstrap";
		case client_state::loading_sandbox:
			return "Loading sandbox";
		case client_state::applying_profile:
			return "Applying profile";
		case client_state::connected:
			return "Connected";
		case client_state::reconnecting:
			return "Reconnecting";
		case client_state::closing:
			return "Closing";
		}
		return "Invalid client state";
	}
}
