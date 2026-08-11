#pragma once

#include "kcd2o.pb.h"
#include "multiplayer/client_state.hpp"

namespace kcd2o
{
	[[nodiscard]] constexpr bool is_server_message_allowed(
	    client_state state,
	    protocol::Envelope::PayloadCase payload) noexcept
	{
		using payload_case = protocol::Envelope::PayloadCase;
		if (!is_valid_client_state(state))
			return false;

		if (payload == payload_case::kPong)
		{
			return state == client_state::preflight
			    || state == client_state::authenticating
			    || state == client_state::downloading_resources
			    || state == client_state::waiting_for_bootstrap
			    || state == client_state::loading_sandbox
			    || state == client_state::applying_profile
			    || state == client_state::connected;
		}
		if (payload == payload_case::kServerRejected
		    || payload == payload_case::kServerShutdown)
		{
			return state != client_state::disconnected
			    && state != client_state::runtime_preflight
			    && state != client_state::connecting
			    && state != client_state::reconnecting
			    && state != client_state::closing;
		}

		switch (state)
		{
		case client_state::preflight:
			return payload == payload_case::kServerChallenge;
		case client_state::authenticating:
			return payload == payload_case::kServerBootstrap
			    || payload == payload_case::kServerResourceManifest;
		case client_state::downloading_resources:
			return payload == payload_case::kServerResourceChunk
			    || payload == payload_case::kServerBootstrap;
		case client_state::waiting_for_bootstrap:
			return payload == payload_case::kServerBootstrap;
		case client_state::applying_profile:
			return payload == payload_case::kServerAccepted;
		case client_state::connected:
			switch (payload)
			{
			case payload_case::kServerHomeMarkerUpdated:
			case payload_case::kPlayerJoined:
			case payload_case::kPlayerLeft:
			case payload_case::kWorldSnapshot:
			case payload_case::kStateCorrection:
			case payload_case::kChatBroadcast:
			case payload_case::kProfileAccepted:
			case payload_case::kProfileRejected:
			case payload_case::kServerEntityControl:
			case payload_case::kAvatarAccepted:
			case payload_case::kAvatarRejected:
			case payload_case::kPlayerAvatarUpdated:
			case payload_case::kWorldObjectAccepted:
			case payload_case::kWorldObjectRejected:
			case payload_case::kWorldObjectUpdated:
			case payload_case::kServerEnvironmentUpdated:
			case payload_case::kWorldItemAccepted:
			case payload_case::kWorldItemRejected:
			case payload_case::kWorldItemUpdated:
			case payload_case::kServerSleepState:
			case payload_case::kServerRespawn:
			case payload_case::kActivityGranted:
			case payload_case::kActivityDenied:
			case payload_case::kPlayerActivityUpdated:
			case payload_case::kServerNpcEnter:
			case payload_case::kServerNpcLeave:
			case payload_case::kServerNpcAuthority:
			case payload_case::kServerNpcSnapshot:
			case payload_case::kServerNpcMotion:
			case payload_case::kServerNpcGameplayUpdate:
			case payload_case::kServerVoiceFrame:
			case payload_case::kServerResourceEvent:
			case payload_case::kServerUiUpdate:
			case payload_case::kServerInputBinding:
				return true;
			default:
				return false;
			}
		default:
			return false;
		}
	}

	// ServerAccepted is delivered reliably on the ordered-state lane. Traffic
	// from the realtime and interactive lanes can overtake it after the server
	// has already promoted the session. Those messages are valid, but the client
	// must discard them until ServerAccepted completes the local transition.
	[[nodiscard]] constexpr bool is_server_message_early_before_accept(
	    client_state state,
	    protocol::Envelope::PayloadCase payload) noexcept
	{
		if (state != client_state::applying_profile)
			return false;
		switch (payload)
		{
		case protocol::Envelope::kWorldSnapshot:
		case protocol::Envelope::kChatBroadcast:
		case protocol::Envelope::kServerNpcSnapshot:
		case protocol::Envelope::kServerNpcMotion:
		case protocol::Envelope::kServerVoiceFrame:
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] constexpr bool server_message_requires_game_thread(protocol::Envelope::PayloadCase payload) noexcept
	{
		using payload_case = protocol::Envelope::PayloadCase;
		switch (payload)
		{
		case payload_case::kServerChallenge:
		case payload_case::kServerRejected:
		case payload_case::kServerShutdown:
		case payload_case::kPong:
		case payload_case::kChatBroadcast:   return false;
		case payload_case::kServerVoiceFrame: return false;
		case payload_case::kServerResourceManifest:
		case payload_case::kServerResourceChunk:
		case payload_case::kServerResourceEvent:
		case payload_case::kServerUiUpdate:
		case payload_case::kServerInputBinding: return false;
		default:                             return true;
		}
	}
} // namespace kcd2o
