#include "multiplayer/client.hpp"

#include "account/account_api.hpp"
#include "account/account_store.hpp"
#include "multiplayer/avatar_visual.hpp"
#include "multiplayer/client_message_gate.hpp"
#include "multiplayer/world_catalog.hpp"
#include "kcse/join_trace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace kcd2o
{
	namespace
	{
		constexpr std::array reconnect_delays{
		    std::chrono::seconds(1),
		    std::chrono::seconds(2),
		    std::chrono::seconds(4),
		    std::chrono::seconds(8),
		    std::chrono::seconds(8)};
		constexpr auto environment_correction_interval =
		    std::chrono::seconds{1};

		std::chrono::milliseconds weather_refresh_interval(float time_scale)
		{
			if (time_scale <= 0.0F)
				return std::chrono::minutes{5};
			// Vanilla may choose another preset after four game hours. Reassert
			// the authoritative profile after at most half that interval.
			const auto milliseconds = static_cast<std::int64_t>(
			    7'200'000.0 / static_cast<double>(time_scale));
			return std::chrono::milliseconds{std::clamp<std::int64_t>(
			    milliseconds,
			    2'000,
			    300'000)};
		}

		void update_environment_status(
		    client_status &status,
		    const protocol::EnvironmentState &environment)
		{
			status.environment_available = true;
			status.time_of_day_hours = environment.time_of_day_hours();
			status.time_scale = environment.time_scale();
			status.weather_id = environment.weather_id();
		}

		std::uint64_t milliseconds(std::chrono::steady_clock::time_point value)
		{
			return static_cast<std::uint64_t>(
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        value.time_since_epoch())
			        .count());
		}

		float distance(
		    const protocol::Vec3 &left,
		    const protocol::Vec3 &right)
		{
			return std::sqrt(
			    std::pow(left.x() - right.x(), 2.0F)
			    + std::pow(left.y() - right.y(), 2.0F)
			    + std::pow(left.z() - right.z(), 2.0F));
		}

		const char *state_name(client_state state)
		{
			switch (state)
			{
			case client_state::disconnected:
				return "disconnected";
			case client_state::runtime_preflight:
				return "runtime-preflight";
			case client_state::connecting:
				return "connecting";
			case client_state::preflight:
				return "protocol-preflight";
			case client_state::authenticating:
				return "authenticating";
			case client_state::waiting_for_bootstrap:
				return "waiting-for-bootstrap";
			case client_state::loading_sandbox:
				return "loading-sandbox";
			case client_state::applying_profile:
				return "applying-profile";
			case client_state::connected:
				return "connected";
			case client_state::reconnecting:
				return "reconnecting";
			case client_state::closing:
				return "closing";
			}
			return "unknown";
		}

		const char *envelope_name(const protocol::Envelope &envelope)
		{
			switch (envelope.payload_case())
			{
			case protocol::Envelope::kClientHello: return "ClientHello";
			case protocol::Envelope::kServerAccepted: return "ServerAccepted";
			case protocol::Envelope::kServerRejected: return "ServerRejected";
			case protocol::Envelope::kPlayerJoined: return "PlayerJoined";
			case protocol::Envelope::kPlayerLeft: return "PlayerLeft";
			case protocol::Envelope::kClientTransform: return "ClientTransform";
			case protocol::Envelope::kWorldSnapshot: return "WorldSnapshot";
			case protocol::Envelope::kStateCorrection: return "StateCorrection";
			case protocol::Envelope::kChatSend: return "ChatSend";
			case protocol::Envelope::kChatBroadcast: return "ChatBroadcast";
			case protocol::Envelope::kPing: return "Ping";
			case protocol::Envelope::kPong: return "Pong";
			case protocol::Envelope::kServerShutdown: return "ServerShutdown";
			case protocol::Envelope::kServerChallenge: return "ServerChallenge";
			case protocol::Envelope::kClientAuthenticate:
				return "ClientAuthenticate";
			case protocol::Envelope::kServerBootstrap: return "ServerBootstrap";
			case protocol::Envelope::kClientWorldReady: return "ClientWorldReady";
			case protocol::Envelope::kClientWorldFailed:
				return "ClientWorldFailed";
			case protocol::Envelope::kClientProfileUpdate:
				return "ClientProfileUpdate";
			case protocol::Envelope::kProfileAccepted: return "ProfileAccepted";
			case protocol::Envelope::kProfileRejected: return "ProfileRejected";
			case protocol::Envelope::kServerEntityControl:
				return "ServerEntityControl";
			case protocol::Envelope::kClientAvatarUpdate:
				return "ClientAvatarUpdate";
			case protocol::Envelope::kAvatarAccepted: return "AvatarAccepted";
			case protocol::Envelope::kAvatarRejected: return "AvatarRejected";
			case protocol::Envelope::kPlayerAvatarUpdated:
				return "PlayerAvatarUpdated";
			case protocol::Envelope::kClientWorldObjectUpdate:
				return "ClientWorldObjectUpdate";
			case protocol::Envelope::kWorldObjectAccepted:
				return "WorldObjectAccepted";
			case protocol::Envelope::kWorldObjectRejected:
				return "WorldObjectRejected";
			case protocol::Envelope::kWorldObjectUpdated:
				return "WorldObjectUpdated";
			case protocol::Envelope::kServerEnvironmentUpdated:
				return "ServerEnvironmentUpdated";
			case protocol::Envelope::kClientWorldItemUpdate:
				return "ClientWorldItemUpdate";
			case protocol::Envelope::kWorldItemAccepted:
				return "WorldItemAccepted";
			case protocol::Envelope::kWorldItemRejected:
				return "WorldItemRejected";
			case protocol::Envelope::kWorldItemUpdated: return "WorldItemUpdated";
			case protocol::Envelope::kClientSleepState: return "ClientSleepState";
			case protocol::Envelope::kServerSleepState: return "ServerSleepState";
			case protocol::Envelope::kClientDeath: return "ClientDeath";
			case protocol::Envelope::kClientRespawnRequest:
				return "ClientRespawnRequest";
			case protocol::Envelope::kServerRespawn: return "ServerRespawn";
			case protocol::Envelope::kClientActivityStart:
				return "ClientActivityStart";
			case protocol::Envelope::kClientActivityEnd:
				return "ClientActivityEnd";
			case protocol::Envelope::kActivityGranted: return "ActivityGranted";
			case protocol::Envelope::kActivityDenied: return "ActivityDenied";
			case protocol::Envelope::kPlayerActivityUpdated:
				return "PlayerActivityUpdated";
			case protocol::Envelope::kServerHomeMarkerUpdated:
				return "ServerHomeMarkerUpdated";
			case protocol::Envelope::kClientNpcDiscovery:
				return "ClientNpcDiscovery";
			case protocol::Envelope::kClientNpcUpdate: return "ClientNpcUpdate";
			case protocol::Envelope::kServerNpcEnter: return "ServerNpcEnter";
			case protocol::Envelope::kServerNpcLeave: return "ServerNpcLeave";
			case protocol::Envelope::kServerNpcAuthority:
				return "ServerNpcAuthority";
			case protocol::Envelope::kServerNpcSnapshot:
				return "ServerNpcSnapshot";
			case protocol::Envelope::kClientNpcUpdateBatch:
				return "ClientNpcUpdateBatch";
			case protocol::Envelope::kServerNpcMotion:
				return "ServerNpcMotion";
			case protocol::Envelope::kServerNpcGameplayUpdate:
				return "ServerNpcGameplayUpdate";
			case protocol::Envelope::kClientVoiceFrame:
				return "ClientVoiceFrame";
			case protocol::Envelope::kServerVoiceFrame:
				return "ServerVoiceFrame";
			case protocol::Envelope::PAYLOAD_NOT_SET: return "PayloadNotSet";
			}
			return "InvalidEnvelopePayload";
		}

		bool same_persistent_profile(
		    protocol::PlayerProfile left,
		    protocol::PlayerProfile right)
		{
			auto normalize = [](protocol::PlayerProfile &profile)
			{
				profile.set_player_id(0);
				profile.set_revision(0);
				profile.clear_display_name();
				profile.clear_level_id();
				profile.clear_last_transform();
				profile.set_transform_valid(false);
				profile.clear_avatar();
			};
			normalize(left);
			normalize(right);
			return left.SerializeAsString() == right.SerializeAsString();
		}

		bool same_item_stack(
		    const protocol::InventoryItem &left,
		    const protocol::InventoryItem &right,
		    bool compare_count = true)
		{
			return left.definition_id() == right.definition_id()
			    && (!compare_count || left.count() == right.count())
			    && left.quality() == right.quality()
			    && left.condition() == right.condition();
		}

		bool inventory_correction_is_destructive(
		    const protocol::PlayerProfile &observed,
		    const protocol::PlayerProfile &authoritative)
		{
			for (const auto &item : observed.inventory())
			{
				const auto match = std::ranges::find_if(
				    authoritative.inventory(),
				    [&](const protocol::InventoryItem &candidate)
				    { return candidate.instance_id() == item.instance_id(); });
				if (match == authoritative.inventory().end()
				    || match->definition_id() != item.definition_id()
				    || match->count() != item.count()
				    || match->quality() != item.quality()
				    || match->condition() != item.condition()
				    || match->has_equipped_slot() != item.has_equipped_slot()
				    || (item.has_equipped_slot()
				        && match->equipped_slot() != item.equipped_slot()))
					return true;
			}
			if (observed.quick_access_slots_size()
			    != authoritative.quick_access_slots_size())
				return true;
			for (const auto &slot : observed.quick_access_slots())
			{
				const auto match = std::ranges::find_if(
				    authoritative.quick_access_slots(),
				    [&](const protocol::QuickAccessSlot &candidate)
				    {
					    return candidate.outfit() == slot.outfit()
					        && candidate.type() == slot.type()
					        && candidate.slot() == slot.slot();
				    });
				if (match == authoritative.quick_access_slots().end()
				    || match->instance_id() != slot.instance_id())
					return true;
			}
			return false;
		}
	}

	multiplayer_client::multiplayer_client(client_runtime &runtime) :
	    m_runtime(runtime)
	{
	}

	multiplayer_client::~multiplayer_client()
	{
		m_network_thread.request_stop();
		queue_network(disconnect_command{});
		if (m_network_thread.joinable())
		{
			m_network_thread.join();
		}
	}

	bool multiplayer_client::connect(client_options options)
	{
		const auto trace_id =
		    kcse::join_trace::begin_join(options.address);
		KCD2Online_JOIN_TRACE(
		    "join.request.validating",
		    std::format(
		        "trace={} display_name_length={} content_hash_length={} "
		        "claim_code_present={}",
		        trace_id,
		        options.display_name.size(),
		        options.content_hash.size(),
		        !options.claim_code.empty()));
		if (!is_valid_display_name(options.display_name)
		    || options.address.empty())
		{
			KCD2Online_JOIN_TRACE(
			    "join.request.rejected",
			    "invalid display name or empty server target");
			kcse::join_trace::finish_join("request validation failed");
			return false;
		}
		if (!m_runtime.can_start_join())
		{
			KCD2Online_JOIN_TRACE(
			    "join.runtime-gate.rejected",
			    "can_start_join=false");
			std::scoped_lock lock(m_state_mutex);
			m_status.error =
			    "KCSE is not ready to connect from the native menu yet.";
			kcse::join_trace::finish_join(m_status.error);
			return false;
		}
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state != client_state::disconnected)
			{
				KCD2Online_JOIN_TRACE(
				    "join.request.rejected",
				    std::format(
				        "client_state={}",
				        state_name(m_status.state)));
				kcse::join_trace::finish_join(
				    "client was not disconnected");
				return false;
			}
		}
		KCD2Online_JOIN_TRACE(
		    "join.runtime.prepare.begin",
		    "calling client_runtime::prepare_multiplayer");
		if (!m_runtime.prepare_multiplayer())
		{
			const auto gate = m_runtime.capability();
			std::scoped_lock lock(m_state_mutex);
			m_status.error = gate.diagnostic.empty()
			    ? "Multiplayer runtime initialization could not start."
			    : gate.diagnostic;
			KCD2Online_JOIN_TRACE(
			    "join.runtime.prepare.failed",
			    m_status.error);
			kcse::join_trace::finish_join(m_status.error);
			return false;
		}
		KCD2Online_JOIN_TRACE(
		    "join.runtime.prepare.accepted",
		    "native capability probe requested");
		{
			std::scoped_lock lock(m_state_mutex);
			m_status = {};
			m_update_rates = {};
			m_manual_disconnect_pending = false;
			m_remote_players.clear();
			{
				std::scoped_lock chat_lock(m_chat_mutex);
				m_chat.clear();
			}
			m_local_correction.reset();
			m_profile.reset();
			m_pending_profile.reset();
			m_world_objects.clear();
			m_pending_world_objects.clear();
			m_deferred_world_objects.clear();
			m_world_items.clear();
			m_pending_world_items.clear();
			m_deferred_world_items.clear();
			m_npcs.clear();
			m_npc_by_guid.clear();
			m_npc_motion_revisions.clear();
			m_human_npcs_disabled = false;
			m_animal_npcs_disabled = false;
			m_last_npc_sampled = {};
			m_last_npc_discovery_sent = {};
			m_local_avatar.reset();
			m_pending_avatar.reset();
			m_desired_avatar.reset();
			m_desired_archetype.reset();
			m_local_activity.reset();
			m_pending_activity_start.reset();
			m_activity_denial.reset();
			m_pending_bootstrap.reset();
			m_profile_update_pending = false;
			m_avatar_update_pending  = false;
			m_last_transform_sent    = {};
			m_last_sent_transform.reset();
			m_last_profile_sent                 = {};
			m_last_avatar_sent                  = {};
			m_last_avatar_sampled               = {};
			m_profile_snapshot_interval_seconds = 15;
			m_environment_revision              = 0;
			m_weather_revision                  = 0;
			m_sleep_revision                    = 0;
			m_last_environment_applied          = {};
			m_last_weather_applied              = {};
			m_resume_token.clear();
			m_pending_connect = std::move(options);
			if (!transition_state_locked(client_state::runtime_preflight))
				return false;
		}
		KCD2Online_JOIN_TRACE(
		    "join.state.initialized",
		    "state=runtime-preflight; client caches cleared");
		ensure_network_thread();
		KCD2Online_JOIN_TRACE(
		    "join.network-thread.ready",
		    "connect will be queued after native preflight");
		return true;
	}

	void multiplayer_client::disconnect()
	{
		KCD2Online_CRITICAL_TRACE(
		    "join.disconnect.requested",
		    "client disconnect requested; transport close deferred to game thread");
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state == client_state::disconnected
			    || m_status.state == client_state::closing)
			{
				return;
			}
			m_manual_disconnect_pending = true;
			if (!transition_state_locked(client_state::closing))
				return;
			m_status.error.clear();
			m_pending_connect.reset();
			m_remote_players.clear();
			m_local_correction.reset();
		}
	}

	void multiplayer_client::fail(std::string error)
	{
		KCD2Online_JOIN_TRACE(
		    "join.client.fail",
		    error);
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state == client_state::disconnected
			    || m_status.state == client_state::closing)
			{
				return;
			}
			if (!transition_state_locked(
			        client_state::closing,
			        std::move(error)))
			{
				return;
			}
		}
		queue_network(disconnect_command{});
	}

	bool multiplayer_client::send_chat(std::string text)
	{
		if (!is_valid_chat(text)
		    || !m_chat_connected.load(std::memory_order_acquire))
		{
			return false;
		}
		queue_network(chat_command{std::move(text)});
		return true;
	}

	bool multiplayer_client::select_avatar(std::string archetype_id)
	{
		std::scoped_lock lock(m_state_mutex);
		if (m_status.state != client_state::connected
		    || !m_local_avatar
		    || std::ranges::find(
		           m_status.avatar_policy.allowed_archetype_ids(),
		           archetype_id)
		        == m_status.avatar_policy.allowed_archetype_ids().end())
		{
			return false;
		}
		m_desired_archetype = std::move(archetype_id);
		return true;
	}

	bool multiplayer_client::set_sleeping(bool sleeping)
	{
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state != client_state::connected || m_status.dead
			    || m_status.sleeping == sleeping)
				return false;
			m_status.sleeping = sleeping;
		}
		queue_network(sleep_command{sleeping});
		return true;
	}

	void multiplayer_client::report_local_death()
	{
		bool should_report{};
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state == client_state::connected && !m_status.dead)
			{
				m_status.dead = true;
				m_status.respawn_pending = false;
				m_status.sleeping = false;
				should_report = true;
			}
		}
		if (should_report)
		{
			queue_network(sleep_command{false});
			queue_network(death_command{});
		}
	}

	bool multiplayer_client::request_respawn()
	{
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state != client_state::connected || !m_status.dead
			    || m_status.respawn_pending)
				return false;
			m_status.respawn_pending = true;
		}
		queue_network(respawn_command{});
		return true;
	}

	bool multiplayer_client::begin_local_activity(
	    protocol::PlayerActivityKind kind,
	    std::uint64_t station_guid)
	{
		protocol::ClientActivityStart message;
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state != client_state::connected || m_status.dead
			    || kind == protocol::PLAYER_ACTIVITY_KIND_NONE
			    || station_guid == 0 || m_local_activity
			    || m_pending_activity_start)
			{
				return false;
			}
			message.set_kind(kind);
			message.set_station_guid(station_guid);
			m_pending_activity_start = message;
			m_activity_denial.reset();
		}
		queue_network(activity_start_command{std::move(message)});
		return true;
	}

	bool multiplayer_client::end_local_activity(
	    std::optional<protocol::TransformState> final_transform)
	{
		protocol::ClientActivityEnd message;
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state != client_state::connected || !m_local_activity
			    || !m_local_activity->active())
			{
				return false;
			}
			message.set_session_id(m_local_activity->session_id());
			if (final_transform)
				*message.mutable_final_transform() = std::move(*final_transform);
		}
		queue_network(activity_end_command{std::move(message)});
		return true;
	}

	std::optional<std::string> multiplayer_client::take_activity_denial()
	{
		std::scoped_lock lock(m_state_mutex);
		auto result = std::move(m_activity_denial);
		m_activity_denial.reset();
		return result;
	}

	void multiplayer_client::runtime_epoch_changed()
	{
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_pending_bootstrap
			    && m_status.state == client_state::loading_sandbox)
			{
				m_remote_players.clear();
				m_local_correction.reset();
				KCD2Online_JOIN_TRACE(
				    "join.runtime-epoch.expected",
				    std::format(
				        "session_id={} target_level={}",
				        m_pending_bootstrap->session_id(),
				        m_pending_bootstrap->level_id()));
				return;
			}
		}
		m_game_commands.clear();
		constexpr std::string_view epoch_error =
		    "KCD2 runtime epoch changed; native handles and queued commands "
		    "were invalidated.";
		if (m_runtime.sandbox_active())
			m_runtime.end_sandbox(epoch_error);
		else
			m_runtime.cancel_multiplayer_preparation();
		{
			std::scoped_lock lock(m_state_mutex);
			m_remote_players.clear();
			m_local_correction.reset();
			m_pending_bootstrap.reset();
			m_world_objects.clear();
			m_pending_world_objects.clear();
			m_deferred_world_objects.clear();
			m_world_items.clear();
			m_pending_world_items.clear();
			m_deferred_world_items.clear();
			m_npcs.clear();
			m_npc_by_guid.clear();
			m_npc_motion_revisions.clear();
			m_environment_revision = 0;
			m_weather_revision = 0;
			m_sleep_revision = 0;
			m_last_environment_applied = {};
			m_last_weather_applied = {};
			m_pending_avatar.reset();
			m_desired_avatar.reset();
			m_local_activity.reset();
			m_pending_activity_start.reset();
			m_activity_denial.reset();
			m_profile.reset();
			m_pending_profile.reset();
			m_local_avatar.reset();
			m_profile_update_pending = false;
			m_avatar_update_pending = false;
			m_pending_connect.reset();
			if (m_status.state == client_state::disconnected)
			{
				m_status.error.clear();
				return;
			}
			if (!transition_state_locked(
			        client_state::closing,
			        std::string(epoch_error)))
			{
				return;
			}
		}
		queue_network(disconnect_command{});
	}

	bool multiplayer_client::reserve_local_avatar_sample(
	    std::chrono::steady_clock::time_point now)
	{
		std::scoped_lock lock(m_state_mutex);
		if (m_status.state != client_state::connected || !m_local_avatar
		    || m_avatar_update_pending)
		{
			return false;
		}
		if (m_last_avatar_sampled
		        != std::chrono::steady_clock::time_point{}
		    && now - m_last_avatar_sampled < std::chrono::milliseconds(250))
		{
			return false;
		}
		m_last_avatar_sampled = now;
		return true;
	}

	void multiplayer_client::game_tick(
	    std::optional<protocol::TransformState> local_transform,
	    std::optional<protocol::AvatarDescriptor> local_avatar_visual,
	    std::string_view current_level,
	    std::chrono::steady_clock::time_point now)
	{
		advance_runtime_preflight();
		bool manual_disconnect{};
		{
			std::scoped_lock lock(m_state_mutex);
			manual_disconnect = m_manual_disconnect_pending;
			m_manual_disconnect_pending = false;
		}
		if (manual_disconnect)
		{
			m_runtime.set_voice_active(false);
			m_runtime.reset_voice();
			KCD2Online_CRITICAL_TRACE(
			    "join.disconnect.game-thread.begin",
			    "voice stopped; queuing transport close without optional native profile capture");
			queue_network(disconnect_command{});
			KCD2Online_CRITICAL_TRACE(
			    "join.disconnect.game-thread.complete",
			    "transport close queued");
			return;
		}
		for (const auto &envelope : m_game_commands.drain())
		{
			handle_game_envelope(envelope, now);
		}
		advance_sandbox_bootstrap();

		bool connected = false;
		bool profile_due = false;
		std::uint32_t tick_rate = 30;
		std::optional<protocol::ClientAvatarUpdate> avatar_update;
		std::string expected_level;
		{
			std::scoped_lock lock(m_state_mutex);
			connected = m_status.state == client_state::connected;
			tick_rate = std::clamp(m_update_rates.tick_rate, 1U, 120U);
			expected_level = m_status.level_id;
			profile_due = connected && !m_profile_update_pending
			    && (m_last_profile_sent
			            == std::chrono::steady_clock::time_point{}
			        || now - m_last_profile_sent
			            >= std::chrono::seconds(
			                m_profile_snapshot_interval_seconds));
			if (profile_due)
				m_last_profile_sent = now;
			m_status.game_queue_size = m_game_commands.size();
			if (connected && m_local_avatar)
			{
				auto desired = merge_avatar_visual(
				    *m_local_avatar,
				    local_avatar_visual,
				    m_desired_archetype);
				m_desired_avatar = desired;
				const bool rate_due =
				    m_last_avatar_sent
				            == std::chrono::steady_clock::time_point{}
				    || now - m_last_avatar_sent
				        >= std::chrono::milliseconds(250);
				if (!m_avatar_update_pending && rate_due
				    && !same_avatar_visual(desired, *m_local_avatar))
				{
					protocol::ClientAvatarUpdate update;
					update.set_base_revision(m_local_avatar->revision());
					*update.mutable_avatar() = desired;
					update.mutable_avatar()->set_revision(
					    update.base_revision());
					m_pending_avatar = update.avatar();
					m_avatar_update_pending = true;
					m_last_avatar_sent = now;
					avatar_update = std::move(update);
				}
			}
		}
		if (avatar_update)
			queue_network(avatar_command{std::move(*avatar_update)});
		m_runtime.set_voice_active(connected);
		if (connected && canonical_level_id(current_level) != canonical_level_id(expected_level))
		{
			set_state(client_state::disconnected, "loaded level no longer matches the server");
			queue_network(disconnect_command{});
			return;
		}
		const auto transform_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / tick_rate));
		if (connected && local_transform && (m_last_transform_sent == std::chrono::steady_clock::time_point{} || now - m_last_transform_sent >= transform_interval))
		{
			auto outgoing = std::move(*local_transform);
			if (m_last_sent_transform && m_last_transform_sent != std::chrono::steady_clock::time_point{})
			{
				const auto elapsed = std::chrono::duration<float>(now - m_last_transform_sent).count();
				if (elapsed >= 0.001F)
				{
					const auto &previous = m_last_sent_transform->position();
					const auto &current  = outgoing.position();
					auto *velocity       = outgoing.mutable_velocity();
					velocity->set_x((current.x() - previous.x()) / elapsed);
					velocity->set_y((current.y() - previous.y()) / elapsed);
					velocity->set_z((current.z() - previous.z()) / elapsed);
				}
			}
			m_last_sent_transform = outgoing;
			queue_network(transform_command{std::move(outgoing)});
			m_last_transform_sent = now;
		}
		std::vector<protocol::WorldObjectState> world_objects;
		std::vector<protocol::WorldItemState> world_items;
		std::vector<protocol::NpcObservation> npc_observations;
		if (connected)
		{
			world_objects = m_runtime.poll_world_object_updates();
			world_items   = m_runtime.poll_world_item_updates();
			if (m_last_npc_sampled == std::chrono::steady_clock::time_point{} || now - m_last_npc_sampled >= std::chrono::milliseconds(200))
			{
				npc_observations   = m_runtime.poll_npc_observations();
				m_last_npc_sampled = now;
			}
		}
		const bool item_transaction_pending =
		    !world_objects.empty() || !world_items.empty();
		if (profile_due && !item_transaction_pending)
		{
			if (const auto profile = m_runtime.local_profile())
			{
				queue_profile_snapshot(*profile);
			}
		}
		if (connected)
		{
			for (auto &voice : m_runtime.poll_outbound_voice())
				queue_network(voice_command{std::move(voice)});
			queue_world_object_updates(std::move(world_objects));
			queue_world_item_updates(std::move(world_items));
			queue_npc_observations(std::move(npc_observations), now);
			if (item_transaction_pending)
				m_last_profile_sent = now;
		}
		update_interpolation(now);
	}

	void multiplayer_client::advance_runtime_preflight()
	{
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state != client_state::runtime_preflight
			    || !m_pending_connect)
				return;
		}

		const auto gate = m_runtime.capability();
		KCD2Online_JOIN_TRACE(
		    "join.runtime.preflight.poll",
		    std::format(
		        "available={} pending={} diagnostic=\"{}\"",
		        gate.available,
		        gate.pending,
		        gate.diagnostic));
		if (!gate.available)
		{
			if (gate.pending)
				return;
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state == client_state::runtime_preflight
			    && m_pending_connect)
			{
				const auto error = gate.diagnostic.empty()
				    ? "Multiplayer runtime initialization failed."
				    : gate.diagnostic;
				(void)transition_state_locked(
				    client_state::disconnected,
				    error);
				KCD2Online_JOIN_TRACE(
				    "join.runtime.preflight.failed",
				    error);
				kcse::join_trace::finish_join(error);
			}
			return;
		}

		std::optional<client_options> options;
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state != client_state::runtime_preflight
			    || !m_pending_connect)
				return;
			options = std::move(m_pending_connect);
			m_pending_connect.reset();
			if (!transition_state_locked(client_state::connecting))
				return;
		}
		KCD2Online_JOIN_TRACE(
		    "join.runtime.preflight.complete",
		    std::format("target=\"{}\"", options->address));
		queue_network(connect_command{std::move(*options)});
		KCD2Online_JOIN_TRACE(
		    "join.network.connect.queued",
		    "connect command enqueued for network thread");
	}

	void multiplayer_client::ensure_network_thread()
	{
		if (m_network_thread.joinable())
		{
			KCD2Online_JOIN_TRACE(
			    "join.network-thread.reuse",
			    "existing std::jthread is joinable");
			return;
		}
		KCD2Online_JOIN_TRACE(
		    "join.network-thread.create",
		    "starting std::jthread");
		m_network_thread = std::jthread(
		    [this](std::stop_token stop)
		    {
			    network_loop(stop);
		    });
	}

	client_status multiplayer_client::status() const
	{
		std::scoped_lock lock(m_state_mutex);
		return m_status;
	}

	client_update_rates multiplayer_client::update_rates() const
	{
		std::scoped_lock lock(m_state_mutex);
		return m_update_rates;
	}

	std::vector<remote_player_view> multiplayer_client::players() const
	{
		std::scoped_lock lock(m_state_mutex);
		std::vector<remote_player_view> result;
		result.reserve(m_remote_players.size() + 1);
		if (m_status.local_player_id != 0 && m_profile)
		{
			remote_player_view local;
			local.id = m_status.local_player_id;
			local.persistent_id = m_profile->persistent_id();
			local.display_name = m_profile->display_name();
			local.connected = m_status.state == client_state::connected;
			local.network_role = m_status.network_role;
			result.push_back(std::move(local));
		}
		for (const auto &[id, player] : m_remote_players)
		{
			(void)id;
			result.push_back(player.rendered);
		}
		std::ranges::sort(result, {}, &remote_player_view::id);
		return result;
	}

	std::vector<remote_player_view> multiplayer_client::remote_players() const
	{
		std::scoped_lock lock(m_state_mutex);
		std::vector<remote_player_view> result;
		result.reserve(m_remote_players.size());
		for (const auto &[id, player] : m_remote_players)
		{
			(void)id;
			result.push_back(player.rendered);
		}
		std::ranges::sort(result, {}, &remote_player_view::id);
		return result;
	}

	std::vector<chat_entry> multiplayer_client::chat_history() const
	{
		std::scoped_lock lock(m_chat_mutex);
		return {m_chat.begin(), m_chat.end()};
	}

	std::optional<protocol::TransformState>
	multiplayer_client::take_local_correction()
	{
		std::scoped_lock lock(m_state_mutex);
		auto correction = std::move(m_local_correction);
		m_local_correction.reset();
		return correction;
	}

	void multiplayer_client::network_loop(std::stop_token stop)
	{
		using namespace std::chrono_literals;
		kcse::join_trace::set_thread_role(
		    kcse::join_trace::thread_role::network);
		try
		{
			KCD2Online_JOIN_TRACE(
			    "join.network-loop.enter",
			    "GameNetworkingSockets runtime construction begins");
			net::runtime runtime;
			KCD2Online_JOIN_TRACE(
			    "join.network-loop.runtime-ready",
			    "GameNetworkingSockets runtime constructed");
			std::optional<net::client_transport> transport;
			bool transport_needs_reset = false;
			bool first_world_snapshot_seen = false;
			client_options options;
			std::size_t reconnect_attempt = 0;
			auto reconnect_at = std::chrono::steady_clock::time_point{};
			auto last_ping = std::chrono::steady_clock::time_point{};

			auto send_envelope =
			    [&](const protocol::Envelope &envelope, reliability delivery)
			{
				if (!transport || !transport->has_connection())
				{
					KCD2Online_JOIN_TRACE(
					    "join.network.send.skipped",
					    std::format(
					        "message={} reason=no-active-connection",
					        envelope_name(envelope)));
					return false;
				}
				std::string error;
				const auto encoded = encode(envelope, delivery, &error);
				if (!encoded)
				{
					KCD2Online_JOIN_TRACE(
					    "join.network.encode.failed",
					    std::format(
					        "message={} error=\"{}\"",
					        envelope_name(envelope),
					        error));
					return false;
				}
				KCD2Online_JOIN_TRACE(
				    "join.network.send.begin",
				    std::format(
				        "message={} bytes={} reliability={}",
				        envelope_name(envelope),
				        encoded->bytes.size(),
				        delivery == reliability::reliable
				            ? "reliable"
				            : "unreliable"));
				const auto sent =
				    transport->send(
				        encoded->bytes,
				        delivery,
				        lane_for(envelope),
				        &error);
				KCD2Online_JOIN_TRACE(
				    sent ? "join.network.send.ok"
				         : "join.network.send.failed",
				    std::format(
				        "message={} error=\"{}\"",
				        envelope_name(envelope),
				        error));
				return sent;
			};

			auto create_transport = [&]
			{
				KCD2Online_JOIN_TRACE(
				    "join.transport.create.begin",
				    "constructing client transport and callbacks");
				transport.emplace(net::client_callbacks{
				    .connected =
				        [&]
				        {
					        KCD2Online_JOIN_TRACE(
					            "join.transport.connected",
					            std::format(
					                "target=\"{}\"; building ClientHello",
					                options.address));
					        if (!set_state(client_state::preflight))
						        return;
					        protocol::Envelope envelope;
					        auto *hello = envelope.mutable_client_hello();
					        hello->set_version(kcd2o_version);
					        hello->set_whgame_timestamp(supported_whgame_timestamp);
					        hello->set_whgame_image_size(
					            supported_whgame_image_size);
					        hello->set_display_name(options.display_name);
					        hello->set_password(options.password);
					        hello->set_content_hash(options.content_hash);
					        auto *runtime_info = hello->mutable_runtime();
					        const auto runtime = m_runtime.descriptor();
					        runtime_info->set_features(runtime.capabilities);
					        runtime_info->set_kcse_version(runtime.kcse_version);
					        runtime_info->set_game_version(runtime.game_version);
					        runtime_info->set_release_index(runtime.release_index);
					        runtime_info->set_runtime_epoch(runtime.epoch);
					        runtime_info->set_address_library(
					            runtime.address_library);
					        runtime_info->set_address_library_distribution(
					            runtime.address_library_distribution);
					        runtime_info->set_address_library_format(
					            runtime.address_library_format);
					        runtime_info->set_address_library_entries(
					            runtime.address_library_entries);
					        runtime_info->set_address_library_sha256(
					            runtime.address_library_sha256);
					        KCD2Online_JOIN_TRACE(
					            "join.handshake.client-hello.ready",
					            std::format(
					                "version={} game={} release={} "
					                "epoch={} capabilities=0x{:X} addresslib=\"{}:{}\" "
					                "format={} entries={} sha256={}",
					                kcd2o_version,
					                runtime.game_version,
					                runtime.release_index,
					                runtime.epoch,
					                runtime.capabilities,
					                runtime.address_library_distribution,
					                runtime.address_library,
					                runtime.address_library_format,
					                runtime.address_library_entries,
					                runtime.address_library_sha256));
					        if (!send_envelope(envelope, reliability::reliable))
					        {
						        set_state(
						            client_state::disconnected,
						            "failed to send ClientHello");
					        }
				        },
				    .disconnected =
				        [&](bool retry, std::string reason)
				        {
					        KCD2Online_JOIN_TRACE(
					            "join.transport.disconnected",
					            std::format(
					                "retry={} attempt={} reason=\"{}\"",
					                retry,
					                reconnect_attempt,
					                reason));
					        transport_needs_reset = true;
					        client_state current_state{};
					        {
						        std::scoped_lock lock(m_state_mutex);
						        current_state = m_status.state;
					        }
					        if (current_state == client_state::closing
					            || current_state == client_state::disconnected)
					        {
						        set_state(client_state::disconnected);
					        }
					        else if (retry
					            && reconnect_attempt < reconnect_delays.size())
					        {
						        reconnect_at = std::chrono::steady_clock::now()
						            + reconnect_delays[reconnect_attempt++];
						        set_state(client_state::reconnecting, reason);
					        }
					        else
					        {
						        set_state(client_state::disconnected, reason);
					        }
				        },
				    .message =
				        [&](std::span<const std::byte> bytes)
				        {
					        KCD2Online_JOIN_TRACE(
					            "join.network.receive.begin",
					            std::format("bytes={}", bytes.size()));
					        std::string error;
					        const auto envelope = decode(bytes, &error);
					        if (!envelope)
					        {
						        KCD2Online_JOIN_TRACE(
						            "join.network.decode.failed",
						            error);
						        set_state(
						            client_state::disconnected,
						            "server sent malformed data");
						        if (transport)
						        {
							        transport->abort_connection(
							            "malformed server message");
						        }
						        return;
					        }
					        KCD2Online_JOIN_TRACE(
					            "join.network.receive.decoded",
					            std::format(
					                "message={}",
					                envelope_name(*envelope)));
					        const auto *message_kind =
					            envelope_name(*envelope);
					        client_state receive_state{};
					        {
						        std::scoped_lock lock(m_state_mutex);
						        receive_state = m_status.state;
					        }
					        if (is_server_message_early_before_accept(
					                receive_state,
					                envelope->payload_case()))
					        {
						        KCD2Online_JOIN_TRACE(
						            "join.protocol.early-message-dropped",
						            std::format(
						                "message={} state={}",
						                message_kind,
						                state_name(receive_state)));
						        return;
					        }
					        if (!is_server_message_allowed(
					                receive_state,
					                envelope->payload_case()))
					        {
						        const auto violation = std::format(
						            "server sent {} while client was {}",
						            message_kind,
						            state_name(receive_state));
						        KCD2Online_JOIN_TRACE(
						            "join.protocol.phase-violation",
						            violation);
						        set_state(client_state::closing, violation);
						        if (transport)
						        {
							        transport->abort_connection(violation);
						        }
						        return;
					        }
					        if (envelope->has_server_accepted())
					        {
						        const auto &accepted =
						            envelope->server_accepted();
						        {
							        std::scoped_lock lock(m_state_mutex);
							        if (!transition_state_locked(
							                client_state::connected))
							        {
								        return;
							        }
							        m_status.local_player_id =
							            accepted.player_id();
							        m_status.server_name =
							            accepted.server_name();
							        m_status.network_role =
							            accepted.network_role();
							        m_status.effective_permissions.assign(
							            accepted.effective_permissions().begin(),
							            accepted.effective_permissions().end());
							        m_status.error_code.clear();
							        m_status.restriction_scope.clear();
							        m_status.restriction_kind.clear();
							        m_status.restriction_reason.clear();
							        m_status.restriction_expires_at_unix_ms = 0;
							        m_status.restriction_reference_id.clear();
							        m_status.support_url.clear();
							        m_status.level_id = accepted.level_id();
							        m_update_rates.tick_rate =
							            accepted.tick_rate();
							        m_update_rates.snapshot_rate =
							            accepted.snapshot_rate();
							        m_status.error.clear();
							        m_profile_snapshot_interval_seconds =
							            accepted
							                .profile_snapshot_interval_seconds();
							        m_resume_token = accepted.resume_token();
						        }
						        KCD2Online_JOIN_TRACE(
						            "join.handshake.server-accepted",
						            std::format(
						                "player_id={} server=\"{}\" level=\"{}\" "
						                "initial_players={}",
						                accepted.player_id(),
						                accepted.server_name(),
						                accepted.level_id(),
						                accepted.players_size()));
						        reconnect_attempt = 0;
					        }
					        else if (envelope->has_server_challenge())
					        {
						        const auto &challenge =
						            envelope->server_challenge();
						        const auto server_id = challenge.server_id();
						        const auto runtime = m_runtime.descriptor();
						        if ((challenge.negotiated_runtime_features()
						                & ~runtime.capabilities)
						            != 0)
						        {
							        set_state(
							            client_state::disconnected,
							            "server negotiated unavailable runtime features");
							        if (transport)
								        transport->abort_connection(
								            "invalid runtime capability negotiation");
							        return;
						        }
						        KCD2Online_JOIN_TRACE(
						            "join.handshake.server-challenge",
						            std::format(
						                "server_id=\"{}\" required=0x{:X} negotiated=0x{:X}",
						                server_id,
						                challenge.required_runtime_features(),
						                challenge.negotiated_runtime_features()));
						        {
							        std::scoped_lock lock(m_state_mutex);
							        m_server_id = server_id;
							        m_status.server_id = server_id;
							        if (!transition_state_locked(
							                client_state::authenticating))
							        {
								        return;
							        }
						        }
					        protocol::Envelope authentication;
					        auto *message =
					            authentication.mutable_client_authenticate();
					        if (challenge.central_auth_required())
					        {
						        if ((!options.server_id.empty()
						                && options.server_id != server_id)
						            || options.account_service_url.empty())
						        {
							        set_state(client_state::disconnected, "server identity does not match the selected browser entry");
							        if (transport) transport->abort_connection("server identity mismatch");
							        return;
						        }
						        try
						        {
							        account::account_store account_store;
							        account::account_api account_api(options.account_service_url);
							        const auto login = account_api.login(account_store.value(), server_id);
							        message->set_access_token(login.access_token);
						        }
						        catch (const std::exception &exception)
						        {
							        set_state(client_state::disconnected, std::string("KCD2Online login failed: ") + exception.what());
							        if (transport) transport->abort_connection("KCD2Online login failed");
							        return;
						        }
						        KCD2Online_JOIN_TRACE("join.handshake.auth.method", "central-account-token");
					        }
					        else if (!options.claim_code.empty())
						        {
							        message->set_claim_code(options.claim_code);
							        KCD2Online_JOIN_TRACE(
							            "join.handshake.auth.method",
							            "claim-code");
						        }
						        else if (const auto token =
						                     m_identities.token_for(server_id))
						        {
							        message->set_identity_token(*token);
							        KCD2Online_JOIN_TRACE(
							            "join.handshake.auth.method",
							            "stored-identity-token");
						        }
						        else
						        {
							        message->set_enroll(true);
							        KCD2Online_JOIN_TRACE(
							            "join.handshake.auth.method",
							            "enrollment");
						        }
						        {
							        std::scoped_lock lock(m_state_mutex);
							        message->set_resume_token(m_resume_token);
						        }
						        if (!send_envelope(
						                authentication,
						                reliability::reliable))
						        {
							        set_state(
							            client_state::disconnected,
							            "failed to send ClientAuthenticate");
						        }
					        }
					        else if (envelope->has_server_bootstrap())
					        {
						        const auto &bootstrap =
						            envelope->server_bootstrap();
						        KCD2Online_JOIN_TRACE(
						            "join.handshake.server-bootstrap",
						            std::format(
						                "server_id=\"{}\" session_id=\"{}\" "
						                "level=\"{}\" mode={} profile={} "
						                "spawn_valid={} has_spawn={} "
						                "profile_transform_valid={} "
						                "profile_has_transform={} "
						                "identity_token_issued={}",
						                bootstrap.server_id(),
						                bootstrap.session_id(),
						                bootstrap.level_id(),
						                static_cast<int>(bootstrap.mode()),
						                bootstrap.has_profile(),
						                bootstrap.spawn_valid(),
						                bootstrap.has_spawn(),
						                bootstrap.has_profile()
						                    && bootstrap.profile().transform_valid(),
						                bootstrap.has_profile()
						                    && bootstrap.profile().has_last_transform(),
						                !bootstrap.issued_identity_token().empty()));
						        if (!bootstrap.issued_identity_token().empty())
						        {
							        m_identities.store(
							            bootstrap.server_id(),
							            bootstrap.issued_identity_token());
						        }
						        {
							        std::scoped_lock lock(m_state_mutex);
							        m_status.server_id = bootstrap.server_id();
							        m_status.session_id = bootstrap.session_id();
							        m_status.level_id = bootstrap.level_id();
							        const auto next_state = bootstrap.mode()
							                == protocol::BOOTSTRAP_MODE_WAIT
							            ? client_state::waiting_for_bootstrap
							            : client_state::loading_sandbox;
							        if (!transition_state_locked(next_state))
								        return;
						        }
					        }
						else if (envelope->has_server_rejected())
						{
							const auto &rejected = envelope->server_rejected();
							KCD2Online_JOIN_TRACE("join.handshake.server-rejected", rejected.message());
							{
								std::scoped_lock lock(m_state_mutex);
								m_status.error_code = rejected.error_code();
								m_status.restriction_scope = rejected.restriction_scope();
								m_status.restriction_kind = rejected.restriction_kind();
								m_status.restriction_reason = rejected.restriction_reason();
								m_status.restriction_expires_at_unix_ms = rejected.expires_at_unix_ms();
								m_status.restriction_reference_id = rejected.reference_id();
								m_status.support_url = rejected.support_url();
							}
							set_state(client_state::disconnected, rejected.message());
						        if (transport)
						        {
							        transport->abort_connection("server rejected connection");
						        }
					        }
					        else if (envelope->has_server_shutdown())
					        {
						        const auto reason = envelope->server_shutdown().reason();
						        KCD2Online_JOIN_TRACE("join.server.shutdown", reason);
						        set_state(client_state::disconnected, reason);
						        if (transport)
						        {
							        transport->abort_connection("server shutdown");
						        }
					        }
					        else if (envelope->has_world_snapshot() && !first_world_snapshot_seen)
					        {
						        first_world_snapshot_seen = true;
						        KCD2Online_JOIN_TRACE("join.snapshot.first-received",
						                          std::format("players={} server_time_ms={}",
						                                      envelope->world_snapshot().players_size(),
						                                      envelope->world_snapshot().server_time_ms()));
					        }
					        else if (envelope->has_chat_broadcast())
					        {
						        // Chat has no native game-thread work. Publish it directly
						        // from the network callback so a busy KCSE command queue cannot
						        // delay messages behind world or NPC updates.
						        const auto &message = envelope->chat_broadcast();
						        std::scoped_lock lock(m_chat_mutex);
						        m_chat.push_back({message.player_id(), message.display_name(), message.text(), message.server_time_ms(), message.channel(), message.network_role()});
						        while (m_chat.size() > 200)
						        {
							        m_chat.pop_front();
						        }
								return;
					        }
					        else if (envelope->has_server_voice_frame())
					        {
								m_runtime.receive_voice(
								    envelope->server_voice_frame());
								return;
					        }

					        if (!server_message_requires_game_thread(envelope->payload_case()))
					        {
						        return;
					        }

					        const bool reliable = !envelope->has_world_snapshot()
					            && !envelope->has_server_npc_snapshot()
					            && !envelope->has_server_npc_motion();
					        if (!m_game_commands.push(std::move(*envelope), reliable) && reliable)
					        {
						        KCD2Online_JOIN_TRACE("join.game-queue.push.failed", std::format("message={} reliable=true", message_kind));
						        set_state(client_state::disconnected, "game-thread queue overflow");
						        if (transport)
						        {
							        transport->abort_connection(
							            "client queue overflow");
						        }
					        }
					        else
					        {
						        KCD2Online_JOIN_TRACE(
						            "join.game-queue.push.ok",
						            std::format(
						                "message={} reliable={}",
						                message_kind,
						                reliable));
					        }
				        }});
				KCD2Online_JOIN_TRACE(
				    "join.transport.create.ok",
				    "client transport callbacks installed");
			};

			while (!stop.stop_requested())
			{
				std::deque<network_command> commands;
				{
					std::scoped_lock lock(m_network_mutex);
					commands.swap(m_network_commands);
				}
				for (auto &command : commands)
				{
					std::visit(
					    [&](auto &typed)
					    {
						    using type = std::decay_t<decltype(typed)>;
						    if constexpr (std::is_same_v<type, connect_command>)
						    {
							    options = std::move(typed.options);
							    reconnect_attempt = 0;
							    first_world_snapshot_seen = false;
							    KCD2Online_JOIN_TRACE(
							        "join.transport.connect.begin",
							        std::format(
							            "target=\"{}\"",
							            options.address));
							    create_transport();
							    transport->connect(options.address);
							    KCD2Online_JOIN_TRACE(
							        "join.transport.connect.started",
							        std::format(
							            "target=\"{}\"",
							            options.address));
						    }
						    else if constexpr (
						        std::is_same_v<type, disconnect_command>)
						    {
							    reconnect_at = {};
							    if (transport)
							    {
								    transport->disconnect();
								    transport.reset();
							    }
							    set_state(client_state::disconnected);
						    }
						    else if constexpr (
						        std::is_same_v<type, transform_command>)
						    {
							    protocol::Envelope envelope;
							    *envelope.mutable_client_transform()
							         ->mutable_transform() =
							        std::move(typed.transform);
							    (void)send_envelope(
							        envelope,
							        reliability::unreliable);
						    }
						    else if constexpr (
						        std::is_same_v<type, chat_command>)
						    {
							    protocol::Envelope envelope;
							    envelope.mutable_chat_send()->set_text(
							        std::move(typed.text));
							    (void)send_envelope(
							        envelope,
							        reliability::reliable);
						    }
						    else if constexpr (
						        std::is_same_v<type, world_ready_command>)
						    {
							    protocol::Envelope envelope;
							    *envelope.mutable_client_world_ready() =
							        std::move(typed.message);
							    (void)send_envelope(
							        envelope,
							        reliability::reliable);
						    }
						    else if constexpr (
						        std::is_same_v<type, world_failed_command>)
						    {
							    protocol::Envelope envelope;
							    *envelope.mutable_client_world_failed() =
							        std::move(typed.message);
							    (void)send_envelope(
							        envelope,
							        reliability::reliable);
						    }
						    else if constexpr (
						        std::is_same_v<type, profile_command>)
						    {
							    protocol::Envelope envelope;
							    *envelope.mutable_client_profile_update() =
							        std::move(typed.message);
							    (void)send_envelope(
							        envelope,
							        reliability::reliable);
						    }
						    else if constexpr (
						        std::is_same_v<type, avatar_command>)
						    {
							    protocol::Envelope envelope;
							    *envelope.mutable_client_avatar_update() =
							        std::move(typed.message);
							    (void)send_envelope(
							        envelope,
							        reliability::reliable);
						    }
						    else if constexpr (
						        std::is_same_v<type, world_object_command>)
						    {
							    protocol::Envelope envelope;
							    *envelope.mutable_client_world_object_update() =
							        std::move(typed.message);
							    (void)send_envelope(
							        envelope,
							        reliability::reliable);
						    }
						    else if constexpr (
						        std::is_same_v<type, world_item_command>)
						    {
							    protocol::Envelope envelope;
							    *envelope.mutable_client_world_item_update() =
							        std::move(typed.message);
							    (void)send_envelope(
							        envelope,
							        reliability::reliable);
						    }
						    else if constexpr (
						        std::is_same_v<type, npc_discovery_command>)
						    {
							    protocol::Envelope envelope;
							    *envelope.mutable_client_npc_discovery() =
							        std::move(typed.message);
							    (void)send_envelope(
							        envelope,
							        reliability::reliable);
						    }
						    else if constexpr (
						        std::is_same_v<type, npc_update_batch_command>)
						    {
							    protocol::Envelope envelope;
							    *envelope.mutable_client_npc_update_batch() =
							        std::move(typed.message);
							    (void)send_envelope(
							        envelope,
							        reliability::unreliable);
						    }
						    else if constexpr (
						        std::is_same_v<type, sleep_command>)
						    {
							    protocol::Envelope envelope;
							    envelope.mutable_client_sleep_state()->set_sleeping(
							        typed.sleeping);
							    (void)send_envelope(
							        envelope,
							        reliability::reliable);
						    }
						    else if constexpr (
						        std::is_same_v<type, death_command>)
						    {
							    protocol::Envelope envelope;
							    envelope.mutable_client_death();
							    (void)send_envelope(
							        envelope,
							        reliability::reliable);
						    }
			else if constexpr (
			    std::is_same_v<type, respawn_command>)
						    {
							    protocol::Envelope envelope;
							    envelope.mutable_client_respawn_request();
							    (void)send_envelope(
							        envelope,
				    reliability::reliable);
			}
			else if constexpr (
			    std::is_same_v<type, activity_start_command>)
			{
				protocol::Envelope envelope;
				*envelope.mutable_client_activity_start() =
				    std::move(typed.message);
				(void)send_envelope(
				    envelope,
				    reliability::reliable);
			}
			else if constexpr (
			    std::is_same_v<type, activity_end_command>)
			{
				protocol::Envelope envelope;
				*envelope.mutable_client_activity_end() =
				    std::move(typed.message);
				(void)send_envelope(
				    envelope,
				    reliability::reliable);
			}
			else if constexpr (
			    std::is_same_v<type, voice_command>)
			{
				protocol::Envelope envelope;
				*envelope.mutable_client_voice_frame() =
				    std::move(typed.message);
				(void)send_envelope(
				    envelope,
				    reliability::unreliable);
			}
					    },
					    command);
				}

				if (!transport
				    && reconnect_at != std::chrono::steady_clock::time_point{}
				    && std::chrono::steady_clock::now() >= reconnect_at)
				{
					KCD2Online_JOIN_TRACE(
					    "join.transport.reconnect.begin",
					    std::format(
					        "target=\"{}\" attempt={}",
					        options.address,
					        reconnect_attempt));
					if (!set_state(client_state::connecting))
					{
						reconnect_at = {};
						continue;
					}
					create_transport();
					transport->connect(options.address);
					KCD2Online_JOIN_TRACE(
					    "join.transport.reconnect.started",
					    std::format(
					        "target=\"{}\"",
					        options.address));
					reconnect_at = {};
				}
				if (transport)
				{
					transport->poll();
					if (transport_needs_reset)
					{
						transport.reset();
						transport_needs_reset = false;
						continue;
					}
					const auto current = std::chrono::steady_clock::now();
					if (transport && transport->has_connection()
					    && (last_ping == std::chrono::steady_clock::time_point{}
					        || current - last_ping >= 3s))
					{
						protocol::Envelope ping;
						ping.mutable_ping()->set_nonce(milliseconds(current));
						ping.mutable_ping()->set_client_time_ms(
						    milliseconds(current));
						(void)send_envelope(ping, reliability::reliable);
						last_ping = current;
					}
					if (transport)
					{
						std::scoped_lock lock(m_state_mutex);
						m_status.ping_ms = transport->ping_ms();
						m_status.packet_loss_percent =
						    transport->packet_loss_percent();
					}
				}
				std::this_thread::sleep_for(1ms);
			}
			if (transport)
			{
				transport->disconnect("KCD2Online client shutting down");
			}
		}
		catch (const std::exception &exception)
		{
			KCD2Online_JOIN_TRACE(
			    "join.network-loop.exception",
			    std::format(
			        "type=std::exception what=\"{}\"",
			        exception.what()));
			set_state(client_state::disconnected, exception.what());
		}
		catch (...)
		{
			KCD2Online_JOIN_TRACE(
			    "join.network-loop.exception",
			    "type=unknown");
			set_state(
			    client_state::disconnected,
			    "unknown exception in network loop");
		}
	}

	bool multiplayer_client::set_state(
	    client_state state,
	    std::string error)
	{
		bool transitioned{};
		std::string final_error;
		{
			std::scoped_lock lock(m_state_mutex);
			transitioned = transition_state_locked(state, std::move(error));
			final_error = m_status.error;
		}
		if (transitioned && state == client_state::disconnected)
		{
			m_runtime.set_voice_active(false);
			m_runtime.reset_voice();
			kcse::join_trace::finish_join(
			    final_error.empty() ? "disconnected" : final_error);
		}
		return transitioned;
	}

	bool multiplayer_client::transition_state_locked(
	    client_state state,
	    std::string error)
	{
		const auto previous = m_status.state;
		if (!is_valid_client_transition(previous, state))
		{
			const auto violation = std::format(
			    "invalid client state transition from {} to {}",
			    state_name(previous),
			    state_name(state));
			KCD2Online_JOIN_TRACE(
			    "join.state.transition-invalid",
			    violation);
			if (previous != client_state::closing
			    && previous != client_state::disconnected)
			{
				m_status.state = client_state::closing;
				m_status.error = violation;
			}
			return false;
		}

		m_status.state = state;
		// A failure first enters closing and then reaches disconnected via
		// intentional transport teardown. Preserve the original cause so the
		// native main menu can still present it after the world is unloaded.
		if (!(state == client_state::disconnected && error.empty()
		        && (previous == client_state::closing
		            || previous == client_state::disconnected)
		        && !m_status.error.empty()))
		{
			m_status.error = std::move(error);
		}
		if (state == client_state::disconnected)
		{
			m_manual_disconnect_pending = false;
			m_pending_bootstrap.reset();
			m_pending_connect.reset();
			m_remote_players.clear();
			m_local_correction.reset();
			m_pending_profile.reset();
			m_pending_avatar.reset();
			m_profile_update_pending = false;
			m_avatar_update_pending = false;
			m_status.local_player_id = 0;
			m_status.network_role = protocol::NETWORK_ROLE_USER;
			m_status.effective_permissions.clear();
			m_status.ping_ms = -1;
			m_status.packet_loss_percent = 0.0F;
			m_status.environment_available = false;
			m_status.time_of_day_hours = 0.0;
			m_status.time_scale = 0.0F;
			m_status.weather_id = 0;
			m_world_objects.clear();
			m_pending_world_objects.clear();
			m_deferred_world_objects.clear();
			m_world_items.clear();
			m_pending_world_items.clear();
			m_deferred_world_items.clear();
			m_npcs.clear();
			m_npc_by_guid.clear();
			m_npc_motion_revisions.clear();
			m_last_npc_sampled = {};
			m_last_npc_discovery_sent = {};
			m_environment_revision = 0;
			m_weather_revision = 0;
			m_sleep_revision = 0;
			m_last_environment_applied = {};
			m_last_weather_applied = {};
			m_status.sleeping = false;
			m_status.sleeping_players = 0;
			m_status.sleeping_players_required = 1;
			m_status.dead = false;
			m_status.respawn_pending = false;
			m_local_activity.reset();
			m_pending_activity_start.reset();
			m_last_environment_applied = {};
		}
		m_chat_connected.store(
		    state == client_state::connected,
		    std::memory_order_release);
		KCD2Online_JOIN_TRACE(
		    "join.state.transition",
		    std::format(
		        "from={} to={} error=\"{}\"",
		        state_name(previous),
		        state_name(state),
		        m_status.error));
		return true;
	}

	void multiplayer_client::queue_network(network_command command)
	{
		std::scoped_lock lock(m_network_mutex);
		if (std::holds_alternative<chat_command>(command))
		{
			const auto first_sync = std::ranges::find_if(
			    m_network_commands,
			    [](const network_command &queued)
			    {
				    return !std::holds_alternative<chat_command>(queued);
			    });
			m_network_commands.insert(first_sync, std::move(command));
		}
		else
		{
			m_network_commands.push_back(std::move(command));
		}
	}

	void multiplayer_client::queue_profile_snapshot(
	    protocol::PlayerProfile profile,
	    bool allow_closing)
	{
		protocol::ClientProfileUpdate update;
		{
			std::scoped_lock lock(m_state_mutex);
			const bool state_allows_update =
			    m_status.state == client_state::connected
			    || (allow_closing
			        && m_status.state == client_state::closing);
			if (!state_allows_update || !m_profile
			    || m_profile_update_pending)
			{
				return;
			}
			profile.set_player_id(m_profile->player_id());
			profile.set_persistent_id(m_profile->persistent_id());
			profile.set_revision(m_profile->revision());
			profile.set_display_name(m_profile->display_name());
			profile.set_level_id(m_profile->level_id());
			if (!is_valid_profile(profile))
			{
				m_status.error =
				    "native profile capture returned an invalid profile";
				return;
			}
			if (same_persistent_profile(profile, *m_profile))
				return;
			update.set_base_revision(m_profile->revision());
			*update.mutable_profile() = std::move(profile);
			m_pending_profile = update.profile();
			m_profile_update_pending = true;
		}
		queue_network(profile_command{std::move(update)});
	}

	void multiplayer_client::queue_world_object_updates(
	    std::vector<protocol::WorldObjectState> updates)
	{
		for (auto &observed : updates)
		{
			protocol::ClientWorldObjectUpdate update;
			{
				std::scoped_lock lock(m_state_mutex);
				if (m_status.state != client_state::connected
				    || !is_valid_world_object_state(observed, false))
				{
					continue;
				}
				if (m_pending_world_objects.contains(observed.entity_guid()))
				{
					m_deferred_world_objects.insert_or_assign(
					    observed.entity_guid(),
					    observed);
					continue;
				}
				const auto current = m_world_objects.find(observed.entity_guid());
				const auto revision = current == m_world_objects.end()
				    ? 0
				    : current->second.revision();
				observed.set_revision(revision);
				if (current != m_world_objects.end()
				    && current->second.SerializeAsString()
				        == observed.SerializeAsString())
				{
					continue;
				}
				update.set_base_revision(revision);
				*update.mutable_state() = observed;
				m_pending_world_objects.insert_or_assign(
				    observed.entity_guid(),
				    observed);
			}
			queue_network(world_object_command{std::move(update)});
		}
	}

	void multiplayer_client::queue_world_item_updates(
	    std::vector<protocol::WorldItemState> updates)
	{
		for (auto &observed : updates)
		{
			protocol::ClientWorldItemUpdate update;
			{
				std::scoped_lock lock(m_state_mutex);
				if (m_status.state != client_state::connected
				    || !is_valid_world_item_state(observed, false))
				{
					continue;
				}
				const auto key = observed.instance_id();
				if (m_pending_world_items.contains(key))
				{
					m_deferred_world_items.insert_or_assign(key, observed);
					continue;
				}
				const auto current = m_world_items.find(key);
				const auto revision = current == m_world_items.end()
				    ? 0
				    : current->second.revision();
				observed.set_revision(revision);
				if (current != m_world_items.end()
				    && current->second.SerializeAsString()
				        == observed.SerializeAsString())
				{
					continue;
				}
				update.set_base_revision(revision);
				*update.mutable_state() = observed;
				if (observed.present() && current == m_world_items.end()
				    && m_profile)
				{
					const protocol::InventoryItem *source = nullptr;
					for (const auto &candidate : m_profile->inventory())
					{
						if (candidate.instance_id() == observed.instance_id()
						    || candidate.count() <= observed.item().count()
						    || !same_item_stack(
						        candidate, observed.item(), false))
						{
							continue;
						}
						if (source)
						{
							source = nullptr;
							break;
						}
						source = &candidate;
					}
					if (source)
					{
						update.set_source_instance_id(source->instance_id());
						update.set_transfer_count(observed.item().count());
					}
				}
				m_pending_world_items.insert_or_assign(key, observed);
			}
			queue_network(world_item_command{std::move(update)});
		}
	}

	void multiplayer_client::queue_npc_observations(
	    std::vector<protocol::NpcObservation> observations,
	    std::chrono::steady_clock::time_point now)
	{
		protocol::ClientNpcDiscovery discovery;
		std::vector<protocol::ClientNpcUpdate> updates;
		std::vector<std::pair<protocol::NpcState, bool>> states_to_apply;
		bool discovery_due{};
		{
			std::scoped_lock lock(m_state_mutex);
			if (m_status.state != client_state::connected)
				return;
			discovery_due =
			    m_last_npc_discovery_sent
			            == std::chrono::steady_clock::time_point{}
			    || now - m_last_npc_discovery_sent >= std::chrono::seconds(1);
			for (auto &observation : observations)
			{
				if (!is_valid_npc_observation(observation)
				    || (observation.kind() == protocol::NPC_KIND_HUMAN
				        && m_human_npcs_disabled)
				    || (observation.kind() == protocol::NPC_KIND_ANIMAL
				        && m_animal_npcs_disabled))
					continue;

				const auto mapped = m_npc_by_guid.find(
				    observation.authored_guid());
				const auto known_id = observation.known_npc_id() != 0
				    ? observation.known_npc_id()
				    : mapped == m_npc_by_guid.end() ? 0 : mapped->second;
				if (known_id == 0)
				{
					if (discovery_due)
						*discovery.add_observations() = std::move(observation);
					continue;
				}
				const auto state = m_npcs.find(known_id);
				if (state == m_npcs.end()
				    || state->second.authority_player_id()
				        != m_status.local_player_id
				    || state->second.lease_id() == 0)
				{
					if (state != m_npcs.end())
						states_to_apply.emplace_back(state->second, false);
					continue;
				}
				states_to_apply.emplace_back(state->second, true);
				protocol::ClientNpcUpdate update;
				update.set_npc_id(state->second.npc_id());
				update.set_generation(state->second.generation());
				update.set_lease_id(state->second.lease_id());
				*update.mutable_transform() = observation.transform();
				*update.mutable_gameplay() = observation.gameplay();
				updates.push_back(std::move(update));
			}
			if (discovery.observations_size() != 0)
				m_last_npc_discovery_sent = now;
		}

		for (int offset = 0; offset < discovery.observations_size();
		     offset += static_cast<int>(max_npcs_per_message))
		{
			protocol::ClientNpcDiscovery chunk;
			const auto count = std::min(
			    static_cast<int>(max_npcs_per_message),
			    discovery.observations_size() - offset);
			for (int index{}; index < count; ++index)
				*chunk.add_observations() = discovery.observations(offset + index);
			queue_network(npc_discovery_command{std::move(chunk)});
		}
		constexpr std::size_t npc_update_batch_budget = 48 * 1024;
		protocol::ClientNpcUpdateBatch batch;
		for (auto &update : updates)
		{
			const auto projected = batch.ByteSizeLong()
			    + update.ByteSizeLong() + 16;
			if (batch.updates_size() != 0
			    && (batch.updates_size()
			            >= static_cast<int>(max_npcs_per_message)
			        || projected > npc_update_batch_budget))
			{
				queue_network(npc_update_batch_command{std::move(batch)});
				batch.Clear();
			}
			*batch.add_updates() = std::move(update);
		}
		if (batch.updates_size() != 0)
			queue_network(npc_update_batch_command{std::move(batch)});

		// Enter can arrive before KCD2 streams the authored Entity. Retrying the
		// latest canonical state when that local GUID is observed binds the native
		// Actor without asking the server to resend full snapshots or spawning a
		// second Actor.
		for (const auto &[state, authority] : states_to_apply)
			(void)m_runtime.apply_npc_state(state, authority);
	}

	void multiplayer_client::handle_game_envelope(
	    const protocol::Envelope &envelope,
	    std::chrono::steady_clock::time_point now)
	{
		KCD2Online_JOIN_TRACE(
		    "join.game-envelope.begin",
		    std::format("message={}", envelope_name(envelope)));
		std::unique_lock lock(m_state_mutex);
		if (m_status.state == client_state::disconnected
		    || m_status.state == client_state::closing)
		{
			KCD2Online_JOIN_TRACE(
			    "join.game-envelope.discarded",
			    std::format(
			        "message={} state={}",
			        envelope_name(envelope),
			        state_name(m_status.state)));
			return;
		}
		if (envelope.has_server_accepted())
		{
			m_status.avatar_policy =
			    envelope.server_accepted().avatar_policy();
			std::optional<protocol::PropertyHomeMarker> home_marker;
			if (envelope.server_accepted().has_home_marker())
				home_marker = envelope.server_accepted().home_marker();
			lock.unlock();
			const bool marker_accepted =
			    m_runtime.set_home_marker(home_marker);
			lock.lock();
			if (!marker_accepted)
				m_status.error = "home marker is waiting for the native map runtime";
			for (const auto &player : envelope.server_accepted().players())
			{
				accept_snapshot_player(player, now, true);
			}
			KCD2Online_JOIN_TRACE(
			    "join.game-envelope.server-accepted.applied",
			    std::format(
			        "players={}",
			        envelope.server_accepted().players_size()));
			kcse::join_trace::finish_join("connected");
			lock.unlock();
			m_runtime.show_multiplayer_notice(
			    "VOIP: V = sprechen, Strg+V = fluestern, Umschalt+V = rufen.");
			lock.lock();
		}
		else if (envelope.has_server_home_marker_updated())
		{
			const auto &updated = envelope.server_home_marker_updated();
			std::optional<protocol::PropertyHomeMarker> marker;
			if (updated.active() && updated.has_marker())
				marker = updated.marker();
			lock.unlock();
			const bool accepted = m_runtime.set_home_marker(marker);
			lock.lock();
			if (!accepted)
				m_status.error = "home marker is waiting for the native map runtime";
		}
		else if (envelope.has_server_bootstrap())
		{
			const auto &bootstrap = envelope.server_bootstrap();
			KCD2Online_JOIN_TRACE(
			    "join.sandbox.bootstrap.begin",
			    std::format(
			        "session_id=\"{}\" level=\"{}\" mode={} profile={} "
			        "spawn_valid={} has_spawn={} profile_transform_valid={} "
			        "profile_has_transform={}",
			        bootstrap.session_id(),
			        bootstrap.level_id(),
			        static_cast<int>(bootstrap.mode()),
			        bootstrap.has_profile(),
			        bootstrap.spawn_valid(),
			        bootstrap.has_spawn(),
			        bootstrap.has_profile()
			            && bootstrap.profile().transform_valid(),
			        bootstrap.has_profile()
			            && bootstrap.profile().has_last_transform()));
			if (m_status.state != client_state::waiting_for_bootstrap
			    && m_status.state != client_state::loading_sandbox)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "server bootstrap reached the game thread in an invalid phase");
				queue_network(disconnect_command{});
				return;
			}
			if (bootstrap.mode() == protocol::BOOTSTRAP_MODE_WAIT)
			{
				return;
			}
			if (!bootstrap.has_profile())
			{
				const std::string error =
				    "server bootstrap did not include a player profile";
				(void)transition_state_locked(
				    client_state::closing,
				    error);
				protocol::ClientWorldFailed failed;
				failed.set_session_id(bootstrap.session_id());
				failed.set_reason(error);
				queue_network(world_failed_command{std::move(failed)});
				return;
			}
			const auto bootstrap_copy = bootstrap;
			lock.unlock();
			const auto result = m_runtime.begin_sandbox(bootstrap_copy);
			KCD2Online_JOIN_TRACE(
			    result.started
			        ? "join.sandbox.bootstrap.started"
			        : "join.sandbox.bootstrap.failed",
			    result.error);
			lock.lock();
			if (!result.started)
			{
				protocol::ClientWorldFailed failed;
				failed.set_session_id(bootstrap.session_id());
				failed.set_reason(result.error);
				queue_network(world_failed_command{std::move(failed)});
				(void)transition_state_locked(
				    client_state::closing,
				    result.error);
				return;
			}
			if (m_status.state == client_state::disconnected
			    || m_status.state == client_state::closing)
			{
				const auto error = m_status.error;
				lock.unlock();
				m_runtime.end_sandbox(error);
				return;
			}
			m_pending_bootstrap = bootstrap_copy;
			m_environment_revision = bootstrap.environment().revision();
			m_weather_revision = bootstrap.environment().weather_revision();
			update_environment_status(m_status, bootstrap.environment());
			m_last_environment_applied = now;
			m_last_weather_applied = now;
			m_world_objects.clear();
			m_pending_world_objects.clear();
			m_deferred_world_objects.clear();
			m_world_items.clear();
			m_pending_world_items.clear();
			m_deferred_world_items.clear();
			m_npcs.clear();
			m_npc_by_guid.clear();
			m_npc_motion_revisions.clear();
			for (const auto &object : bootstrap.world_objects())
				m_world_objects.emplace(object.entity_guid(), object);
			for (const auto &item : bootstrap.world_items())
				m_world_items.emplace(item.instance_id(), item);
		}
		else if (envelope.has_player_joined())
		{
			accept_snapshot_player(
			    envelope.player_joined().player(),
			    now,
			    true);
		}
		else if (envelope.has_player_left())
		{
			m_remote_players.erase(envelope.player_left().player_id());
		}
		else if (envelope.has_world_snapshot())
		{
			const auto &environment = envelope.world_snapshot().environment();
			update_environment_status(m_status, environment);
			const bool environment_changed =
			    environment.revision() > m_environment_revision;
			const bool environment_current =
			    environment.revision() == m_environment_revision;
			const bool environment_correction_due =
			    m_last_environment_applied
			        == std::chrono::steady_clock::time_point{}
			    || now - m_last_environment_applied
			        >= environment_correction_interval;
			const bool weather_changed =
			    environment.weather_revision() > m_weather_revision;
			const bool weather_current =
			    environment.weather_revision() == m_weather_revision;
			const bool weather_refresh_due =
			    m_last_weather_applied
			        == std::chrono::steady_clock::time_point{}
			    || now - m_last_weather_applied
			        >= weather_refresh_interval(environment.time_scale());
			if (environment_changed
			    || (environment_current
			        && (environment_correction_due
			            || (weather_current && weather_refresh_due))))
			{
				const auto state = environment;
				const bool apply_weather =
				    weather_changed
				    || (environment_current && weather_current
				        && weather_refresh_due);
				lock.unlock();
				const bool applied = m_runtime.apply_environment_state(
				    state,
				    apply_weather);
				lock.lock();
				if (!applied)
				{
					(void)transition_state_locked(
					    client_state::closing,
					    "could not apply the server environment timeline");
					queue_network(disconnect_command{});
					return;
				}
				m_environment_revision = std::max(
				    m_environment_revision,
				    state.revision());
				m_last_environment_applied = now;
				m_weather_revision = std::max(
				    m_weather_revision,
				    state.weather_revision());
				if (apply_weather)
					m_last_weather_applied = now;
			}
			KCD2Online_JOIN_TRACE(
			    "join.snapshot.apply.begin",
			    std::format(
			        "players={} server_time_ms={}",
			        envelope.world_snapshot().players_size(),
			        envelope.world_snapshot().server_time_ms()));
			for (const auto &player : envelope.world_snapshot().players())
			{
				accept_snapshot_player(player, now);
			}
			KCD2Online_JOIN_TRACE(
			    "join.snapshot.apply.complete",
			    std::format(
			        "tracked_remote_players={}",
			        m_remote_players.size()));
		}
		else if (envelope.has_server_environment_updated())
		{
			const auto state = envelope.server_environment_updated().state();
			if (state.revision() <= m_environment_revision)
				return;
			update_environment_status(m_status, state);
			const bool apply_weather =
			    state.weather_revision() > m_weather_revision;
			lock.unlock();
			const bool applied = m_runtime.apply_environment_state(
			    state,
			    apply_weather);
			lock.lock();
			if (!applied)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "could not apply the updated server environment");
				queue_network(disconnect_command{});
				return;
			}
			m_environment_revision = state.revision();
			m_weather_revision = state.weather_revision();
			m_last_environment_applied = now;
			if (apply_weather)
				m_last_weather_applied = now;
		}
		else if (envelope.has_server_sleep_state())
		{
			const auto &state = envelope.server_sleep_state();
			if (state.revision() <= m_sleep_revision)
				return;
			m_sleep_revision = state.revision();
			m_status.sleeping_players = state.sleeping_players();
			m_status.sleeping_players_required = state.required_players();
			if (state.time_skipped())
				m_status.sleeping = false;
			const auto notice = state.time_skipped()
			    ? std::string{"Die Nacht wurde uebersprungen."}
			    : std::format(
			          "Schlafende Spieler: {}/{}",
			          state.sleeping_players(),
			          state.required_players());
			lock.unlock();
			if (state.sleeping_players() != 0 || state.time_skipped())
				m_runtime.show_multiplayer_notice(notice);
			lock.lock();
		}
		else if (envelope.has_server_respawn())
		{
			if (!m_status.dead || !m_status.respawn_pending)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "server sent an unsolicited respawn");
				queue_network(disconnect_command{});
				return;
			}
			const auto spawn = envelope.server_respawn().spawn();
			lock.unlock();
			const bool applied = m_runtime.respawn_local_player(spawn);
			lock.lock();
			if (!applied)
			{
				(void)transition_state_locked(client_state::closing, "could not revive the native local player");
				queue_network(disconnect_command{});
				return;
			}
			m_status.dead            = false;
			m_status.respawn_pending = false;
			m_local_correction.reset();
			m_last_sent_transform.reset();
			m_last_transform_sent = {};
		}
		else if (envelope.has_activity_granted())
		{
			const auto &activity = envelope.activity_granted().activity();
			if (!m_pending_activity_start || m_pending_activity_start->kind() != activity.kind()
			    || m_pending_activity_start->station_guid() != activity.station_guid())
			{
				(void)transition_state_locked(client_state::closing, "server granted an unexpected activity session");
				queue_network(disconnect_command{});
				return;
			}
			m_pending_activity_start.reset();
			m_local_activity = activity;
		}
		else if (envelope.has_activity_denied())
		{
			const auto &denied = envelope.activity_denied();
			if (!m_pending_activity_start
			    || m_pending_activity_start->kind() != denied.kind()
			    || m_pending_activity_start->station_guid()
			        != denied.station_guid())
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "server denied an activity that was not pending");
				queue_network(disconnect_command{});
				return;
			}
			m_pending_activity_start.reset();
			m_activity_denial = denied.reason();
			const auto notice = std::string{"Aktivitaet abgelehnt: "}
			    + denied.reason();
			lock.unlock();
			m_runtime.show_multiplayer_notice(notice);
			lock.lock();
		}
		else if (envelope.has_player_activity_updated())
		{
			const auto &message = envelope.player_activity_updated();
			if (message.player_id() == m_status.local_player_id)
			{
				if (message.activity().active())
					m_local_activity = message.activity();
				else
					m_local_activity.reset();
				m_pending_activity_start.reset();
				return;
			}
			auto &remote = m_remote_players[message.player_id()];
			remote.rendered.id           = message.player_id();
			remote.rendered.activity     = message.activity();
			remote.rendered.has_activity = message.activity().active();
			if (message.has_final_transform())
			{
				remote.rendered.transform     = message.final_transform();
				remote.rendered.has_transform = true;
			}
		}
		else if (envelope.has_state_correction())
		{
			m_local_correction = envelope.state_correction().accepted_transform();
			m_last_sent_transform.reset();
			m_last_transform_sent = {};
		}
		else if (envelope.has_profile_accepted())
		{
			if (!m_profile || !m_pending_profile || !m_profile_update_pending
			    || envelope.profile_accepted().revision() != m_profile->revision() + 1)
			{
				(void)transition_state_locked(client_state::closing, "server returned an invalid profile revision");
				queue_network(disconnect_command{});
				return;
			}
			m_pending_profile->set_revision(
			    envelope.profile_accepted().revision());
			m_profile = std::move(m_pending_profile);
			m_pending_profile.reset();
			m_profile_update_pending = false;
		}
		else if (envelope.has_profile_rejected())
		{
			if (!m_profile || !m_pending_profile || !m_profile_update_pending)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "server rejected a profile update that was not pending");
				queue_network(disconnect_command{});
				return;
			}
			const auto authoritative =
			    envelope.profile_rejected().authoritative_profile();
			const auto destructive = inventory_correction_is_destructive(
			    *m_pending_profile, authoritative);
			if (destructive)
			{
				m_pending_profile.reset();
				m_profile_update_pending = false;
				m_status.error =
				    "server rejected an inventory change; session closed without "
				    "mutating the live native inventory: "
				    + envelope.profile_rejected().reason();
				(void)transition_state_locked(client_state::closing, m_status.error);
				queue_network(disconnect_command{});
				return;
			}
			lock.unlock();
			const bool applied =
			    m_runtime.apply_authoritative_profile(authoritative);
			lock.lock();
			m_pending_profile.reset();
			m_profile_update_pending = false;
			if (!applied)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "could not apply authoritative profile correction: "
				        + envelope.profile_rejected().reason());
				queue_network(disconnect_command{});
				return;
			}
			m_profile = authoritative;
			m_status.error = envelope.profile_rejected().reason();
		}
		else if (envelope.has_world_object_accepted())
		{
			const auto &accepted = envelope.world_object_accepted();
			const auto pending =
			    m_pending_world_objects.find(accepted.entity_guid());
			if (pending == m_pending_world_objects.end())
			{
				const auto current = m_world_objects.find(accepted.entity_guid());
				if (current != m_world_objects.end()
				    && current->second.revision() >= accepted.revision())
					return;
				(void)transition_state_locked(
				    client_state::closing,
				    "server accepted an unknown world object revision");
				queue_network(disconnect_command{});
				return;
			}
			if (accepted.revision() != pending->second.revision() + 1)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "server accepted an unknown or invalid world object revision");
				queue_network(disconnect_command{});
				return;
			}
			auto state = pending->second;
			state.set_revision(accepted.revision());
			m_world_objects.insert_or_assign(accepted.entity_guid(), state);
			m_pending_world_objects.erase(pending);
			if (const auto deferred =
			        m_deferred_world_objects.find(accepted.entity_guid());
			    deferred != m_deferred_world_objects.end())
			{
				auto desired = deferred->second;
				m_deferred_world_objects.erase(deferred);
				desired.set_revision(accepted.revision());
				if (desired.SerializeAsString() != state.SerializeAsString())
				{
					protocol::ClientWorldObjectUpdate update;
					update.set_base_revision(accepted.revision());
					*update.mutable_state() = desired;
					m_pending_world_objects.insert_or_assign(
					    accepted.entity_guid(), desired);
					queue_network(world_object_command{std::move(update)});
				}
			}
			if (accepted.has_authoritative_profile())
			{
				const auto authoritative = accepted.authoritative_profile();
				lock.unlock();
				const bool applied =
				    m_runtime.apply_authoritative_profile(authoritative);
				lock.lock();
				if (!applied)
				{
					(void)transition_state_locked(
					    client_state::closing,
					    "could not apply atomic container transfer");
					queue_network(disconnect_command{});
					return;
				}
				m_profile = authoritative;
				m_local_avatar = authoritative.avatar();
			}
		}
		else if (envelope.has_world_object_rejected())
		{
			const auto state =
			    envelope.world_object_rejected().authoritative_state();
			if (!m_pending_world_objects.contains(state.entity_guid()))
			{
				const auto current = m_world_objects.find(state.entity_guid());
				if (current != m_world_objects.end()
				    && current->second.revision() >= state.revision())
					return;
				(void)transition_state_locked(
				    client_state::closing,
				    "server rejected a world object update that was not pending");
				queue_network(disconnect_command{});
				return;
			}
			m_pending_world_objects.erase(state.entity_guid());
			m_deferred_world_objects.erase(state.entity_guid());
			m_world_objects.insert_or_assign(state.entity_guid(), state);
			lock.unlock();
			const bool applied = m_runtime.apply_world_object_state(state);
			lock.lock();
			if (!applied)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "could not apply world object correction");
				queue_network(disconnect_command{});
				return;
			}
		}
		else if (envelope.has_world_object_updated())
		{
			const auto state = envelope.world_object_updated().state();
			const auto current = m_world_objects.find(state.entity_guid());
			if (current != m_world_objects.end()
			    && state.revision() <= current->second.revision())
			{
				return;
			}
			m_pending_world_objects.erase(state.entity_guid());
			m_deferred_world_objects.erase(state.entity_guid());
			m_world_objects.insert_or_assign(state.entity_guid(), state);
			lock.unlock();
			const bool applied = m_runtime.apply_world_object_state(state);
			lock.lock();
			if (!applied)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "could not apply remote world object update");
				queue_network(disconnect_command{});
				return;
			}
		}
		else if (envelope.has_world_item_accepted())
		{
			const auto &accepted = envelope.world_item_accepted();
			const auto pending =
			    m_pending_world_items.find(accepted.instance_id());
			if (pending == m_pending_world_items.end())
			{
				const auto current = m_world_items.find(accepted.instance_id());
				if (current != m_world_items.end()
				    && current->second.revision() >= accepted.revision())
					return;
				(void)transition_state_locked(
				    client_state::closing,
				    "server accepted an unknown world item revision");
				queue_network(disconnect_command{});
				return;
			}
			if (accepted.revision() != pending->second.revision() + 1)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "server accepted an unknown or invalid world item revision");
				queue_network(disconnect_command{});
				return;
			}
			auto state = pending->second;
			state.set_revision(accepted.revision());
			m_world_items.insert_or_assign(accepted.instance_id(), state);
			m_pending_world_items.erase(pending);
			if (const auto deferred =
			        m_deferred_world_items.find(accepted.instance_id());
			    deferred != m_deferred_world_items.end())
			{
				auto desired = deferred->second;
				m_deferred_world_items.erase(deferred);
				desired.set_revision(accepted.revision());
				if (desired.SerializeAsString() != state.SerializeAsString())
				{
					protocol::ClientWorldItemUpdate update;
					update.set_base_revision(accepted.revision());
					*update.mutable_state() = desired;
					m_pending_world_items.insert_or_assign(
					    accepted.instance_id(), desired);
					queue_network(world_item_command{std::move(update)});
				}
			}
			if (accepted.has_authoritative_profile())
			{
				const auto authoritative = accepted.authoritative_profile();
				lock.unlock();
				const bool applied =
				    m_runtime.apply_authoritative_profile(authoritative);
				lock.lock();
				if (!applied)
				{
					(void)transition_state_locked(
					    client_state::closing,
					    "could not apply atomic world item transfer");
					queue_network(disconnect_command{});
					return;
				}
				m_profile = authoritative;
				m_local_avatar = authoritative.avatar();
			}
		}
		else if (envelope.has_world_item_rejected())
		{
			const auto state =
			    envelope.world_item_rejected().authoritative_state();
			if (!m_pending_world_items.contains(state.instance_id()))
			{
				const auto current = m_world_items.find(state.instance_id());
				if (current != m_world_items.end()
				    && current->second.revision() >= state.revision())
					return;
				(void)transition_state_locked(
				    client_state::closing,
				    "server rejected a world item update that was not pending");
				queue_network(disconnect_command{});
				return;
			}
			m_pending_world_items.erase(state.instance_id());
			m_deferred_world_items.erase(state.instance_id());
			m_world_items.insert_or_assign(state.instance_id(), state);
			lock.unlock();
			const bool applied = m_runtime.apply_world_item_state(state);
			lock.lock();
			if (!applied)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "could not apply world item correction");
				queue_network(disconnect_command{});
				return;
			}
		}
		else if (envelope.has_world_item_updated())
		{
			const auto state = envelope.world_item_updated().state();
			const auto current = m_world_items.find(state.instance_id());
			if (current != m_world_items.end()
			    && state.revision() <= current->second.revision())
			{
				return;
			}
			m_pending_world_items.erase(state.instance_id());
			m_deferred_world_items.erase(state.instance_id());
			m_world_items.insert_or_assign(state.instance_id(), state);
			lock.unlock();
			const bool applied = m_runtime.apply_world_item_state(state);
			lock.lock();
			if (!applied)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "could not apply remote world item update");
				queue_network(disconnect_command{});
				return;
			}
		}
		else if (envelope.has_avatar_accepted())
		{
			if (!m_local_avatar || !m_pending_avatar
			    || !m_avatar_update_pending
			    || envelope.avatar_accepted().revision()
			        != m_local_avatar->revision() + 1)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "server returned an invalid avatar revision");
				queue_network(disconnect_command{});
				return;
			}
			m_pending_avatar->set_revision(
			    envelope.avatar_accepted().revision());
			m_local_avatar = *m_pending_avatar;
			m_status.avatar_archetype_id =
			    m_local_avatar->archetype_id();
			m_pending_avatar.reset();
			m_desired_archetype.reset();
			if (m_profile)
			{
				*m_profile->mutable_avatar() = *m_local_avatar;
			}
			m_avatar_update_pending = false;
		}
		else if (envelope.has_avatar_rejected())
		{
			if (!m_local_avatar || !m_pending_avatar || !m_avatar_update_pending)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "server rejected an avatar update that was not pending");
				queue_network(disconnect_command{});
				return;
			}
			m_local_avatar =
			    envelope.avatar_rejected().authoritative_avatar();
			m_status.avatar_archetype_id =
			    m_local_avatar->archetype_id();
			m_pending_avatar.reset();
			m_desired_archetype.reset();
			if (m_profile)
			{
				*m_profile->mutable_avatar() = *m_local_avatar;
			}
			m_avatar_update_pending = false;
			m_status.error = envelope.avatar_rejected().reason();
		}
		else if (envelope.has_player_avatar_updated())
		{
			const auto &message = envelope.player_avatar_updated();
			if (message.player_id() != m_status.local_player_id)
			{
				auto &remote = m_remote_players[message.player_id()];
				remote.rendered.id = message.player_id();
				remote.rendered.avatar = message.avatar();
				remote.rendered.has_avatar = true;
			}
		}
		else if (envelope.has_chat_broadcast())
		{
			const auto &message = envelope.chat_broadcast();
			std::scoped_lock chat_lock(m_chat_mutex);
			m_chat.push_back({
			    message.player_id(),
			    message.display_name(),
			    message.text(),
			    message.server_time_ms(),
			    message.channel(),
			    message.network_role()});
			while (m_chat.size() > 200)
			{
				m_chat.pop_front();
			}
		}
		else if (envelope.has_server_npc_enter())
		{
			const auto state = envelope.server_npc_enter().state();
			const auto current = m_npcs.find(state.npc_id());
			if (current != m_npcs.end()
			    && current->second.generation() > state.generation())
				return;
			m_npcs.insert_or_assign(state.npc_id(), state);
			m_npc_by_guid.insert_or_assign(state.authored_guid(), state.npc_id());
			m_npc_motion_revisions.insert_or_assign(
			    state.npc_id(), state.revision());
			const bool authority =
			    state.authority_player_id() == m_status.local_player_id;
			lock.unlock();
			const bool applied = m_runtime.apply_npc_state(state, authority);
			lock.lock();
			if (!applied)
				m_status.error = "native NPC enter is waiting for level streaming";
		}
		else if (envelope.has_server_npc_leave())
		{
			const auto &message = envelope.server_npc_leave();
			const auto current = m_npcs.find(message.npc_id());
			if (current == m_npcs.end()
			    || current->second.generation() != message.generation())
				return;
			m_npc_by_guid.erase(current->second.authored_guid());
			m_npc_motion_revisions.erase(message.npc_id());
			m_npcs.erase(current);
			lock.unlock();
			m_runtime.remove_npc_state(
			    message.npc_id(), message.generation());
			lock.lock();
		}
		else if (envelope.has_server_npc_authority())
		{
			const auto &message = envelope.server_npc_authority();
			const auto current = m_npcs.find(message.npc_id());
			if (current == m_npcs.end()
			    || current->second.generation() != message.generation())
				return;
			current->second.set_authority_player_id(
			    message.authority_player_id());
			current->second.set_lease_id(message.lease_id());
			const auto state = current->second;
			const bool authority =
			    state.authority_player_id() == m_status.local_player_id;
			lock.unlock();
			(void)m_runtime.apply_npc_state(state, authority);
			lock.lock();
		}
		else if (envelope.has_server_npc_snapshot())
		{
			for (const auto &state : envelope.server_npc_snapshot().npcs())
			{
				const auto current = m_npcs.find(state.npc_id());
				if (current == m_npcs.end()
				    || current->second.generation() != state.generation()
				    || (state.revision() <= current->second.revision()
				        && state.lease_id() == current->second.lease_id()))
					continue;
				auto merged = state;
				if (current != m_npcs.end() && current->second.has_gameplay()
				    && current->second.gameplay().has_inventory()
				    && merged.has_gameplay()
				    && !merged.gameplay().has_inventory())
					*merged.mutable_gameplay()->mutable_inventory() =
					    current->second.gameplay().inventory();
				m_npcs.insert_or_assign(merged.npc_id(), merged);
				m_npc_motion_revisions.insert_or_assign(
				    merged.npc_id(), merged.revision());
				m_npc_by_guid.insert_or_assign(
				    merged.authored_guid(), merged.npc_id());
				const bool authority =
				    merged.authority_player_id() == m_status.local_player_id;
				lock.unlock();
				(void)m_runtime.apply_npc_state(merged, authority);
				lock.lock();
			}
		}
		else if (envelope.has_server_npc_motion())
		{
			for (const auto &motion : envelope.server_npc_motion().npcs())
			{
				const auto current = m_npcs.find(motion.npc_id());
				if (current == m_npcs.end()
				    || current->second.generation() != motion.generation())
					continue;
				auto &motion_revision = m_npc_motion_revisions[motion.npc_id()];
				if (motion.revision() <= motion_revision)
					continue;

				*current->second.mutable_transform() = motion.transform();
				current->second.set_revision(std::max(
				    current->second.revision(), motion.revision()));
				motion_revision = motion.revision();
				const auto state = current->second;
				const bool authority =
				    state.authority_player_id() == m_status.local_player_id;
				lock.unlock();
				(void)m_runtime.apply_npc_state(state, authority);
				lock.lock();
			}
		}
		else if (envelope.has_server_npc_gameplay_update())
		{
			const auto &message = envelope.server_npc_gameplay_update();
			const auto current = m_npcs.find(message.npc_id());
			if (current == m_npcs.end()
			    || current->second.generation() != message.generation()
			    || (current->second.has_gameplay()
			        && message.gameplay().revision()
			            <= current->second.gameplay().revision()))
				return;

			auto gameplay = message.gameplay();
			if (!gameplay.has_inventory()
			    && current->second.has_gameplay()
			    && current->second.gameplay().has_inventory())
				*gameplay.mutable_inventory() =
				    current->second.gameplay().inventory();
			*current->second.mutable_gameplay() = std::move(gameplay);
			current->second.set_revision(std::max(
			    current->second.revision(), message.state_revision()));
			const auto state = current->second;
			const bool authority =
			    state.authority_player_id() == m_status.local_player_id;
			lock.unlock();
			(void)m_runtime.apply_npc_state(state, authority);
			lock.lock();
		}
		else if (envelope.has_server_entity_control())
		{
			const auto &control = envelope.server_entity_control();
			const bool legacy_disabled =
			    control.non_player_entities_disabled();
			const bool humans_disabled = control.has_human_npcs_disabled()
			    ? control.human_npcs_disabled()
			    : legacy_disabled;
			const bool animals_disabled = control.has_animal_npcs_disabled()
			    ? control.animal_npcs_disabled()
			    : legacy_disabled;
			m_human_npcs_disabled = humans_disabled;
			m_animal_npcs_disabled = animals_disabled;
			std::vector<std::pair<std::uint64_t, std::uint32_t>> removed;
			for (auto iterator = m_npcs.begin(); iterator != m_npcs.end();)
			{
				const bool disabled =
				    (iterator->second.kind() == protocol::NPC_KIND_HUMAN
				        && humans_disabled)
				    || (iterator->second.kind() == protocol::NPC_KIND_ANIMAL
				        && animals_disabled);
				if (!disabled)
				{
					++iterator;
					continue;
				}
				removed.emplace_back(
				    iterator->second.npc_id(), iterator->second.generation());
				m_npc_by_guid.erase(iterator->second.authored_guid());
				m_npc_motion_revisions.erase(iterator->second.npc_id());
				iterator = m_npcs.erase(iterator);
			}
			std::vector<protocol::NpcState> remaining;
			remaining.reserve(m_npcs.size());
			for (const auto &[npc_id, state] : m_npcs)
			{
				(void)npc_id;
				remaining.push_back(state);
			}
			const auto local_player_id = m_status.local_player_id;
			lock.unlock();
			for (const auto &[npc_id, generation] : removed)
				m_runtime.remove_npc_state(npc_id, generation);
			const bool applied = m_runtime.set_npc_entities_disabled(
			    humans_disabled,
			    animals_disabled);
			if (applied)
			{
				for (const auto &state : remaining)
					(void)m_runtime.apply_npc_state(
					    state,
					    state.authority_player_id()
					        == local_player_id);
			}
			lock.lock();
			if (!applied)
			{
				(void)transition_state_locked(
				    client_state::closing,
				    "could not apply the server's entity-control state");
				queue_network(disconnect_command{});
			}
		}
		else if (envelope.has_server_shutdown())
		{
			(void)transition_state_locked(
			    client_state::disconnected,
			    envelope.server_shutdown().reason());
			queue_network(disconnect_command{});
		}
	}

	void multiplayer_client::advance_sandbox_bootstrap()
	{
		std::optional<protocol::ServerBootstrap> bootstrap;
		{
			std::scoped_lock lock(m_state_mutex);
			if (!m_pending_bootstrap)
			{
				return;
			}
			bootstrap = *m_pending_bootstrap;
		}

		const auto progress = m_runtime.poll_sandbox();
		if (progress.phase == sandbox_phase::loading)
		{
			return;
		}
		if (progress.phase != sandbox_phase::ready)
		{
			const auto reason = progress.error.empty()
			    ? "sandbox bootstrap stopped before the world became ready"
			    : progress.error;
			protocol::ClientWorldFailed failed;
			failed.set_session_id(bootstrap->session_id());
			failed.set_reason(reason);
			{
				std::scoped_lock lock(m_state_mutex);
				if (!m_pending_bootstrap
				    || m_pending_bootstrap->session_id()
				        != bootstrap->session_id())
				{
					return;
				}
				m_pending_bootstrap.reset();
				(void)transition_state_locked(
				    client_state::closing,
				    reason);
			}
			queue_network(world_failed_command{std::move(failed)});
			m_runtime.end_sandbox(reason);
			return;
		}

		protocol::ClientWorldReady ready;
		ready.set_session_id(bootstrap->session_id());
		ready.set_manifest_revision(bootstrap->manifest_revision());
		ready.set_level_id(bootstrap->level_id());
		ready.set_initialized_session(
		    bootstrap->mode() == protocol::BOOTSTRAP_MODE_INITIALIZE);
		if (progress.initial_spawn)
		{
			*ready.mutable_initial_spawn() = *progress.initial_spawn;
		}
		if (!bootstrap->profile().has_avatar())
		{
			const std::string reason =
			    "server profile has no avatar descriptor";
			protocol::ClientWorldFailed failed;
			failed.set_session_id(bootstrap->session_id());
			failed.set_reason(reason);
			{
				std::scoped_lock lock(m_state_mutex);
				if (!m_pending_bootstrap
				    || m_pending_bootstrap->session_id()
				        != bootstrap->session_id())
				{
					return;
				}
				m_pending_bootstrap.reset();
				(void)transition_state_locked(
				    client_state::closing,
				    reason);
			}
			queue_network(world_failed_command{std::move(failed)});
			m_runtime.end_sandbox(reason);
			return;
		}
		*ready.mutable_avatar() = bootstrap->profile().avatar();
		{
			std::scoped_lock lock(m_state_mutex);
			if (!m_pending_bootstrap
			    || m_pending_bootstrap->session_id()
			        != bootstrap->session_id())
			{
				return;
			}
			m_pending_bootstrap.reset();
			if (!transition_state_locked(client_state::applying_profile))
				return;
			m_profile = bootstrap->profile();
			m_pending_profile.reset();
			m_local_avatar = bootstrap->profile().avatar();
			m_status.avatar_archetype_id =
			    m_local_avatar->archetype_id();
			m_pending_avatar.reset();
			m_desired_avatar.reset();
			m_desired_archetype.reset();
			m_profile_update_pending = false;
			m_avatar_update_pending  = false;
		}
		queue_network(world_ready_command{std::move(ready)});
	}

	void multiplayer_client::update_interpolation(std::chrono::steady_clock::time_point now)
	{
		std::scoped_lock lock(m_state_mutex);
		const auto snapshot_rate       = std::clamp(m_update_rates.snapshot_rate, 1U, 120U);
		const auto interpolation_delay = std::chrono::milliseconds(std::clamp(1000U / snapshot_rate + 10U, 40U, 100U));
		const auto target              = now - interpolation_delay;
		for (auto &[id, player] : m_remote_players)
		{
			(void)id;
			if (player.history.empty())
			{
				continue;
			}
			while (player.history.size() > 2 && player.history[1].received_at <= target)
			{
				player.history.pop_front();
			}

			auto rendered  = player.history.front().transform;
			auto mode      = player.history.front().mode;
			bool connected = player.history.front().connected;
			if (player.history.size() >= 2 && player.history.front().received_at <= target)
			{
				const auto &from    = player.history[0];
				const auto &to      = player.history[1];
				const auto duration = std::chrono::duration<float>(to.received_at - from.received_at).count();
				if (target <= to.received_at)
				{
					const auto elapsed = std::chrono::duration<float>(
					    target - from.received_at).count();
					const auto factor = duration <= 0.0F
					    ? 1.0F
					    : std::clamp(elapsed / duration, 0.0F, 1.0F);
					rendered = interpolate(
					    from.transform, to.transform, factor);
					mode = factor < 0.5F ? from.mode : to.mode;
				}
				else
				{
					// Keep the Avatar moving briefly beyond the newest packet. Prefer
					// its explicit velocity; derive it from the last two rendered
					// samples when an older sender did not provide one.
					auto newest = to.transform;
					const auto wire_speed = std::hypot(
					    newest.velocity().x(), newest.velocity().y());
					if (wire_speed < 0.05F && duration > 0.001F)
					{
						auto *velocity = newest.mutable_velocity();
						velocity->set_x((to.transform.position().x()
						    - from.transform.position().x()) / duration);
						velocity->set_y((to.transform.position().y()
						    - from.transform.position().y()) / duration);
						velocity->set_z((to.transform.position().z()
						    - from.transform.position().z()) / duration);
					}
					const auto seconds = std::clamp(
					    std::chrono::duration<float>(
					        target - to.received_at).count(),
					    0.0F,
					    0.06F);
					rendered = extrapolate(newest, seconds);
					mode = to.mode;
				}
				connected = to.connected;
			}
			else if (player.history.size() == 1
			    && target > player.history.front().received_at)
			{
				const auto seconds = std::min(
				    std::chrono::duration<float>(
				        target - player.history.front().received_at)
				        .count(),
				    0.06F);
				rendered = extrapolate(player.history.front().transform, seconds);
			}

			if (player.rendered.has_transform
			    && distance(
			           player.rendered.transform.position(),
			           rendered.position())
			        > 5.0F)
			{
				player.rendered.transform = rendered;
			}
			else
			{
				player.rendered.transform = std::move(rendered);
			}
			player.rendered.has_transform = true;
			player.rendered.movement_mode = mode;
			player.rendered.connected = connected;
		}
	}

	void multiplayer_client::accept_snapshot_player(
	    const protocol::PlayerSnapshot &snapshot,
	    std::chrono::steady_clock::time_point now,
	    bool reset_transform_stream)
	{
		if (snapshot.player_id() == m_status.local_player_id)
		{
			return;
		}
		auto &player = m_remote_players[snapshot.player_id()];
		if (reset_transform_stream)
		{
			KCD2Online_JOIN_TRACE(
			    "join.remote-transform.stream-reset",
			    std::format(
			        "player_id={} previous_sequence={} buffered={}",
			        snapshot.player_id(),
			        player.transform_sequence.value(),
			        player.history.size()));
			player.history.clear();
			player.transform_sequence.reset();
			player.rendered.has_transform = false;
		}
		player.display_name = snapshot.display_name();
		player.rendered.id = snapshot.player_id();
		player.rendered.persistent_id = snapshot.persistent_id();
		player.rendered.display_name = snapshot.display_name();
		player.rendered.network_role = snapshot.network_role();
		player.rendered.connected = snapshot.connected();
		if (snapshot.has_avatar())
		{
			player.rendered.avatar = snapshot.avatar();
			player.rendered.has_avatar = true;
		}
		player.rendered.has_activity =
		    snapshot.has_activity() && snapshot.activity().active();
		if (snapshot.has_activity())
			player.rendered.activity = snapshot.activity();
		else
			player.rendered.activity.Clear();
		if (!snapshot.transform_valid() || !snapshot.has_transform())
		{
			return;
		}
		const auto sequence = snapshot.transform().sequence();
		if (!player.transform_sequence.accept(sequence))
		{
			return;
		}
		player.history.push_back({
		    now,
		    snapshot.transform(),
		    snapshot.movement_mode(),
		    snapshot.connected()});
		while (player.history.size() > 32)
		{
			player.history.pop_front();
		}
	}

	protocol::TransformState multiplayer_client::interpolate(
	    const protocol::TransformState &from,
	    const protocol::TransformState &to,
	    float factor)
	{
		protocol::TransformState result = to;
		auto lerp = [factor](float left, float right)
		{
			return left + (right - left) * factor;
		};
		result.mutable_position()->set_x(
		    lerp(from.position().x(), to.position().x()));
		result.mutable_position()->set_y(
		    lerp(from.position().y(), to.position().y()));
		result.mutable_position()->set_z(
		    lerp(from.position().z(), to.position().z()));
		result.mutable_velocity()->set_x(
		    lerp(from.velocity().x(), to.velocity().x()));
		result.mutable_velocity()->set_y(
		    lerp(from.velocity().y(), to.velocity().y()));
		result.mutable_velocity()->set_z(
		    lerp(from.velocity().z(), to.velocity().z()));
		if (from.has_locomotion() && to.has_locomotion())
		{
			auto *state = result.mutable_locomotion();
			auto lerp_vec = [&](protocol::Vec3 *output,
			                    const protocol::Vec3 &left,
			                    const protocol::Vec3 &right)
			{
				output->set_x(lerp(left.x(), right.x()));
				output->set_y(lerp(left.y(), right.y()));
				output->set_z(lerp(left.z(), right.z()));
			};
			lerp_vec(
			    state->mutable_local_velocity(),
			    from.locomotion().local_velocity(),
			    to.locomotion().local_velocity());
			lerp_vec(
			    state->mutable_acceleration(),
			    from.locomotion().acceleration(),
			    to.locomotion().acceleration());
			lerp_vec(
			    state->mutable_facing_direction(),
			    from.locomotion().facing_direction(),
			    to.locomotion().facing_direction());
			state->set_speed(lerp(
			    from.locomotion().speed(), to.locomotion().speed()));
			state->set_yaw_rate(lerp(
			    from.locomotion().yaw_rate(),
			    to.locomotion().yaw_rate()));
		}
		auto *rotation = result.mutable_rotation();
		const auto dot = from.rotation().x() * to.rotation().x()
		    + from.rotation().y() * to.rotation().y()
		    + from.rotation().z() * to.rotation().z()
		    + from.rotation().w() * to.rotation().w();
		const auto sign = dot < 0.0F ? -1.0F : 1.0F;
		rotation->set_x(
		    lerp(from.rotation().x(), to.rotation().x() * sign));
		rotation->set_y(
		    lerp(from.rotation().y(), to.rotation().y() * sign));
		rotation->set_z(
		    lerp(from.rotation().z(), to.rotation().z() * sign));
		rotation->set_w(
		    lerp(from.rotation().w(), to.rotation().w() * sign));
		(void)normalize_rotation(rotation);
		return result;
	}

	protocol::TransformState multiplayer_client::extrapolate(
	    const protocol::TransformState &from,
	    float seconds)
	{
		auto result = from;
		result.mutable_position()->set_x(
		    from.position().x() + from.velocity().x() * seconds);
		result.mutable_position()->set_y(
		    from.position().y() + from.velocity().y() * seconds);
		result.mutable_position()->set_z(
		    from.position().z() + from.velocity().z() * seconds);
		return result;
	}

}
