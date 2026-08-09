#include "multiplayer/client_message_gate.hpp"
#include "multiplayer/client_state.hpp"

#include <array>
#include <cassert>
#include <string_view>

int main()
{
	using namespace kcd2o;
	using payload = protocol::Envelope::PayloadCase;

	constexpr std::array states{
	    client_state::disconnected,
	    client_state::runtime_preflight,
	    client_state::connecting,
	    client_state::preflight,
	    client_state::authenticating,
	    client_state::waiting_for_bootstrap,
	    client_state::loading_sandbox,
	    client_state::applying_profile,
	    client_state::connected,
	    client_state::reconnecting,
	    client_state::closing};
	for (const auto state : states)
	{
		assert(is_valid_client_state(state));
		assert(std::string_view(to_string(state)) != "Invalid client state");
	}
	assert(!is_valid_client_state(static_cast<client_state>(999)));

	constexpr std::array happy_path{
	    client_state::disconnected,
	    client_state::runtime_preflight,
	    client_state::connecting,
	    client_state::preflight,
	    client_state::authenticating,
	    client_state::loading_sandbox,
	    client_state::applying_profile,
	    client_state::connected};
	for (std::size_t index = 1; index < happy_path.size(); ++index)
	{
		assert(is_valid_client_transition(
		    happy_path[index - 1],
		    happy_path[index]));
		if (happy_path[index - 1] == client_state::disconnected)
		{
			assert(is_valid_client_transition(
			    happy_path[index],
			    happy_path[index - 1]));
		}
		else
		{
			assert(!is_valid_client_transition(
			    happy_path[index],
			    happy_path[index - 1]));
		}
	}
	assert(is_valid_client_transition(
	    client_state::authenticating,
	    client_state::waiting_for_bootstrap));
	assert(is_valid_client_transition(
	    client_state::waiting_for_bootstrap,
	    client_state::loading_sandbox));
	assert(is_valid_client_transition(
	    client_state::connected,
	    client_state::reconnecting));
	assert(is_valid_client_transition(
	    client_state::reconnecting,
	    client_state::connecting));
	assert(!is_valid_client_transition(
	    client_state::connected,
	    client_state::authenticating));
	assert(!is_valid_client_transition(
	    client_state::closing,
	    client_state::reconnecting));

	assert(is_server_message_allowed(
	    client_state::preflight,
	    payload::kServerChallenge));
	assert(!is_server_message_allowed(
	    client_state::preflight,
	    payload::kServerBootstrap));
	assert(is_server_message_allowed(
	    client_state::authenticating,
	    payload::kServerBootstrap));
	assert(is_server_message_allowed(
	    client_state::waiting_for_bootstrap,
	    payload::kServerBootstrap));
	assert(is_server_message_allowed(
	    client_state::applying_profile,
	    payload::kServerAccepted));
	assert(is_server_message_early_before_accept(
	    client_state::applying_profile,
	    payload::kWorldSnapshot));
	assert(is_server_message_early_before_accept(
	    client_state::applying_profile,
	    payload::kChatBroadcast));
	assert(!is_server_message_early_before_accept(
	    client_state::applying_profile,
	    payload::kServerAccepted));
	assert(!is_server_message_early_before_accept(
	    client_state::applying_profile,
	    payload::kServerEntityControl));
	assert(!is_server_message_early_before_accept(
	    client_state::authenticating,
	    payload::kWorldSnapshot));
	assert(!is_server_message_allowed(
	    client_state::loading_sandbox,
	    payload::kServerAccepted));
	assert(is_server_message_allowed(
	    client_state::connected,
	    payload::kWorldSnapshot));
	assert(is_server_message_allowed(
	    client_state::connected,
	    payload::kServerNpcMotion));
	assert(is_server_message_allowed(
	    client_state::connected,
	    payload::kServerNpcGameplayUpdate));
	assert(!is_server_message_allowed(
	    client_state::authenticating,
	    payload::kWorldSnapshot));

	// A peer acting as a server must never send a client-originated payload.
	for (const auto state : states)
	{
		assert(!is_server_message_allowed(state, payload::kClientHello));
		assert(!is_server_message_allowed(state, payload::kClientProfileUpdate));
	}
	assert(!server_message_requires_game_thread(payload::kServerChallenge));
	assert(!server_message_requires_game_thread(payload::kServerRejected));
	assert(!server_message_requires_game_thread(payload::kServerShutdown));
	assert(!server_message_requires_game_thread(payload::kPong));
	assert(!server_message_requires_game_thread(payload::kChatBroadcast));
	assert(server_message_requires_game_thread(payload::kServerBootstrap));
	assert(server_message_requires_game_thread(payload::kWorldSnapshot));
	return 0;
}
