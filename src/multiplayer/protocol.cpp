#include "multiplayer/protocol.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace kcd2o
{
	namespace
	{
		void set_error(std::string *error, std::string message)
		{
			if (error)
			{
				*error = std::move(message);
			}
		}

		bool finite(float value)
		{
			return std::isfinite(value);
		}

		bool valid_version_label(std::string_view value)
		{
			return !value.empty() && value.size() <= 32
			    && std::ranges::all_of(
			        value,
			        [](unsigned char character)
			        {
				        return std::isalnum(character) != 0
				            || character == '.' || character == '-'
				            || character == '+';
			        });
		}

		bool is_non_combat_animation_fragment(std::string_view value)
		{
			if (value.empty() || value.size() > max_animation_fragment_bytes)
				return false;
			std::string lower(value);
			std::ranges::transform(
			    lower,
			    lower.begin(),
			    [](unsigned char character)
			    {
				    return static_cast<char>(std::tolower(character));
			    });
			constexpr std::string_view excluded[] = {
			    "combat", "attack", "strike", "parry", "block", "hit",
			    "death", "finisher", "motion", "locomotion", "idle",
			    "walk", "run", "sprint"};
			return std::ranges::none_of(
			    excluded,
			    [&](std::string_view token)
			    {
				    return lower.contains(token);
			    });
		}

		bool valid_player_snapshot(
		    const protocol::PlayerSnapshot &player,
		    bool require_avatar)
		{
			return is_valid_display_name(player.display_name())
			    && player.player_id() != 0
			    && (player.persistent_id().empty()
			        || is_uuid(player.persistent_id()))
			    && protocol::MovementMode_IsValid(
			        static_cast<int>(player.movement_mode()))
			    && (!player.transform_valid()
			        || is_finite_transform(player.transform()))
			    && (!require_avatar || player.has_avatar())
			    && (!player.has_avatar()
			        || is_valid_avatar_descriptor(player.avatar()))
			    && (!player.has_activity()
			        || (player.activity().active()
			            && player.activity().kind()
			                != protocol::PLAYER_ACTIVITY_KIND_NONE
			            && protocol::PlayerActivityKind_IsValid(
			                static_cast<int>(player.activity().kind()))
			            && player.activity().station_guid() != 0
			            && player.activity().session_id() != 0
			            && player.activity().revision() != 0));
		}

		bool valid_identifier(std::string_view value, std::size_t maximum = 128)
		{
			return valid_utf8_with_codepoint_count(value, 1, maximum);
		}

		bool valid_inventory_item(
		    const protocol::InventoryItem &item,
		    bool allow_equipped_slot)
		{
			return is_uuid(item.instance_id())
			    && is_uuid(item.definition_id())
			    && item.count() > 0 && item.count() <= max_profile_item_count
			    && std::isfinite(item.quality())
			    && item.quality() >= 0.0F && item.quality() <= 100.0F
			    && std::floor(item.quality()) == item.quality()
			    && std::isfinite(item.condition())
			    && item.condition() >= 0.0F && item.condition() <= 1.0F
			    && (allow_equipped_slot || !item.has_equipped_slot());
		}

		bool valid_npc_gameplay(const protocol::NpcGameplayState &state)
		{
			if (state.revision() == 0 || !std::isfinite(state.health())
			    || !std::isfinite(state.max_health()) || state.health() < 0.0F
			    || state.max_health() <= 0.0F
			    || state.health() > state.max_health() + 0.01F
			    || state.max_health() > 1'000'000.0F
			    || !std::isfinite(state.desired_speed())
			    || state.desired_speed() < 0.0F
			    || state.desired_speed() > 40.0F
			    || (state.has_behavior_target()
			        && (!finite(state.behavior_target().x())
			            || !finite(state.behavior_target().y())
			            || !finite(state.behavior_target().z())))
			    || state.behavior() == protocol::NPC_BEHAVIOR_UNSPECIFIED
			    || state.behavior() > protocol::NPC_BEHAVIOR_DEAD
			    || state.aggro_size() > static_cast<int>(max_players))
				return false;
			std::unordered_set<std::uint64_t> aggro_players;
			for (const auto &entry : state.aggro())
				if (entry.player_id() == 0 || !std::isfinite(entry.value())
				    || entry.value() < 0.0F || entry.value() > 1'000'000.0F
				    || !aggro_players.insert(entry.player_id()).second)
					return false;
			if (state.has_inventory())
			{
				if (state.inventory().revision() == 0
				    || state.inventory().items_size()
				        > static_cast<int>(max_profile_inventory_items))
					return false;
				std::unordered_set<std::string> instances;
				for (const auto &item : state.inventory().items())
					if (!valid_inventory_item(item, false)
					    || !instances.insert(item.instance_id()).second)
						return false;
			}
			if (state.has_dialog())
			{
				const auto &dialog = state.dialog();
				if (dialog.revision() == 0
				    || (dialog.active() && dialog.session_id() == 0))
					return false;
			}
			if (state.has_last_combat_result())
			{
				const auto &result = state.last_combat_result();
				if (result.event_id() == 0
				    || !std::isfinite(result.health_damage())
				    || !std::isfinite(result.stamina_damage())
				    || result.health_damage() < 0.0F
				    || result.stamina_damage() < 0.0F
				    || result.health_damage() > 100'000.0F
				    || result.stamina_damage() > 100'000.0F)
					return false;
			}
			return true;
		}

		bool valid_home_marker(const protocol::PropertyHomeMarker &marker)
		{
			return valid_identifier(marker.property_id())
			    && valid_identifier(marker.level_id())
			    && valid_utf8_with_codepoint_count(
			        marker.display_name(), 1, 128)
			    && marker.has_position()
			    && finite(marker.position().x())
			    && finite(marker.position().y())
			    && finite(marker.position().z())
			    && marker.entity_guid() != 0
			    && (marker.role() == protocol::PROPERTY_ROLE_OWNER
			        || marker.role() == protocol::PROPERTY_ROLE_RESIDENT);
		}

		bool valid_envelope(const protocol::Envelope &envelope)
		{
			if (envelope.has_client_npc_discovery())
			{
				const auto &message = envelope.client_npc_discovery();
				if (message.observations_size() == 0
				    || message.observations_size()
				        > static_cast<int>(max_npcs_per_message))
					return false;
				std::unordered_set<std::uint64_t> guids;
				for (const auto &observation : message.observations())
				{
					if (!is_valid_npc_observation(observation)
					    || !guids.insert(observation.authored_guid()).second)
						return false;
				}
				return true;
			}
			if (envelope.has_client_npc_update())
			{
				const auto &message = envelope.client_npc_update();
				return message.npc_id() != 0 && message.generation() != 0
				    && message.lease_id() != 0 && message.has_transform()
				    && is_finite_transform(message.transform())
				    && (!message.has_gameplay()
				        || valid_npc_gameplay(message.gameplay()));
			}
			if (envelope.has_client_npc_update_batch())
			{
				const auto &message = envelope.client_npc_update_batch();
				if (message.updates_size() == 0
				    || message.updates_size()
				        > static_cast<int>(max_npcs_per_message))
					return false;
				std::unordered_set<std::uint64_t> ids;
				for (const auto &update : message.updates())
					if (update.npc_id() == 0 || update.generation() == 0
					    || update.lease_id() == 0 || !update.has_transform()
					    || !is_finite_transform(update.transform())
					    || (update.has_gameplay()
					        && !valid_npc_gameplay(update.gameplay()))
					    || !ids.insert(update.npc_id()).second)
						return false;
				return true;
			}
			if (envelope.has_server_npc_enter())
				return envelope.server_npc_enter().has_state()
				    && is_valid_npc_state(envelope.server_npc_enter().state());
			if (envelope.has_server_npc_leave())
			{
				const auto &message = envelope.server_npc_leave();
				return message.npc_id() != 0 && message.generation() != 0;
			}
			if (envelope.has_server_npc_authority())
			{
				const auto &message = envelope.server_npc_authority();
				return message.npc_id() != 0 && message.generation() != 0
				    && ((message.authority_player_id() == 0
				            && message.lease_id() == 0)
				        || (message.authority_player_id() != 0
				            && message.lease_id() != 0));
			}
			if (envelope.has_server_npc_snapshot())
			{
				const auto &message = envelope.server_npc_snapshot();
				if (message.server_tick() == 0
				    || message.npcs_size() > static_cast<int>(max_npcs_per_message))
					return false;
				std::unordered_set<std::uint64_t> ids;
				for (const auto &state : message.npcs())
				{
					if (!is_valid_npc_state(state)
					    || !ids.insert(state.npc_id()).second)
						return false;
				}
				return true;
			}
			if (envelope.has_server_npc_motion())
			{
				const auto &message = envelope.server_npc_motion();
				if (message.server_tick() == 0 || message.npcs_size() == 0
				    || message.npcs_size()
				        > static_cast<int>(max_npcs_per_message))
					return false;
				std::unordered_set<std::uint64_t> ids;
				for (const auto &motion : message.npcs())
					if (motion.npc_id() == 0 || motion.generation() == 0
					    || motion.revision() == 0 || !motion.has_transform()
					    || !is_finite_transform(motion.transform())
					    || !ids.insert(motion.npc_id()).second)
						return false;
				return true;
			}
			if (envelope.has_server_npc_gameplay_update())
			{
				const auto &message = envelope.server_npc_gameplay_update();
				return message.npc_id() != 0 && message.generation() != 0
				    && message.state_revision() != 0 && message.has_gameplay()
				    && valid_npc_gameplay(message.gameplay());
			}
			if (envelope.has_client_hello())
			{
				const auto &message = envelope.client_hello();
				return valid_version_label(message.version())
				    && message.whgame_timestamp()
				        == supported_whgame_timestamp
				    && message.whgame_image_size()
				        == supported_whgame_image_size
				    && is_valid_display_name(message.display_name())
				    && message.password().size() <= 256
				    && message.content_hash().size() <= 64
				    && message.resume_token().size() <= 128
				    && message.has_runtime()
				    && (message.runtime().features()
				            & ~known_client_runtime_capabilities)
				        == 0
				    && message.runtime().runtime_epoch() != 0
				    && message.runtime().kcse_version() != 0
				    && message.runtime().game_version()
				        == supported_kcse_game_version
				    && message.runtime().release_index()
				        == supported_kcse_release_index
				    && is_valid_address_library_identity(message.runtime());
			}
			if (envelope.has_server_challenge())
			{
				const auto &message = envelope.server_challenge();
				return valid_identifier(message.server_id())
				    && message.required_runtime_features()
				        == required_client_runtime_capabilities
				    && (message.negotiated_runtime_features()
				            & ~known_client_runtime_capabilities)
				        == 0
				    && (message.negotiated_runtime_features()
				            & message.required_runtime_features())
				        == message.required_runtime_features();
			}
			if (envelope.has_client_authenticate())
			{
				const auto &message = envelope.client_authenticate();
				const auto credential_count =
				    static_cast<int>(!message.identity_token().empty())
				    + static_cast<int>(!message.claim_code().empty())
				    + static_cast<int>(message.enroll());
				return credential_count == 1
				    && message.identity_token().size() <= 128
				    && message.claim_code().size() <= 64
				    && message.resume_token().size() <= 128;
			}
			if (envelope.has_server_bootstrap())
			{
				const auto &message = envelope.server_bootstrap();
				return valid_identifier(message.server_id())
				    && valid_identifier(message.session_id())
				    && valid_identifier(message.level_id())
				    && message.manifest_revision() > 0
				    && message.timeout_seconds() >= 30
				    && message.timeout_seconds() <= 600
				    && protocol::BootstrapMode_IsValid(
				        static_cast<int>(message.mode()))
				    && (!message.spawn_valid()
				        || (message.has_spawn()
				            && is_finite_transform(message.spawn())))
				    && (!message.has_profile()
				        || is_valid_profile(message.profile()))
				    && message.issued_identity_token().size() <= 128
				    && message.world_objects_size()
				        <= static_cast<int>(max_world_objects)
				    && message.world_items_size()
				        <= static_cast<int>(max_world_items)
				    && message.has_environment()
				    && is_valid_environment_state(message.environment())
				    && std::ranges::all_of(
				        message.world_objects(),
				        [](const protocol::WorldObjectState &state)
				        { return is_valid_world_object_state(state); })
				    && std::ranges::all_of(
				        message.world_items(),
				        [](const protocol::WorldItemState &state)
				        { return is_valid_world_item_state(state); });
			}
			if (envelope.has_client_world_ready())
			{
				const auto &message = envelope.client_world_ready();
				return valid_identifier(message.session_id())
				    && valid_identifier(message.level_id())
				    && message.manifest_revision() > 0
				    && message.has_avatar()
				    && is_valid_avatar_descriptor(message.avatar())
				    && (!message.initialized_session()
				        || (message.has_initial_spawn()
				            && is_finite_transform(message.initial_spawn())));
			}
			if (envelope.has_client_world_failed())
			{
				const auto &message = envelope.client_world_failed();
				return valid_identifier(message.session_id())
				    && valid_utf8_with_codepoint_count(
				        message.reason(), 1, 512);
			}
			if (envelope.has_client_profile_update())
			{
				return envelope.client_profile_update().has_profile()
				    && envelope.client_profile_update().base_revision() > 0
				    && is_valid_profile(
				        envelope.client_profile_update().profile());
			}
			if (envelope.has_profile_accepted())
			{
				return envelope.profile_accepted().revision() > 0;
			}
			if (envelope.has_profile_rejected())
			{
				return envelope.profile_rejected().authoritative_revision() > 0
				    && envelope.profile_rejected().has_authoritative_profile()
				    && is_valid_profile(
				        envelope.profile_rejected().authoritative_profile())
				    && valid_utf8_with_codepoint_count(
				        envelope.profile_rejected().reason(), 1, 512);
			}
			if (envelope.has_client_world_object_update())
			{
				const auto &message = envelope.client_world_object_update();
				return message.has_state()
				    && is_valid_world_object_state(message.state(), false)
				    && message.state().revision() == message.base_revision();
			}
			if (envelope.has_world_object_accepted())
			{
				const auto &message = envelope.world_object_accepted();
				return message.entity_guid() != 0 && message.revision() > 0
				    && (!message.has_authoritative_profile()
				        || is_valid_profile(message.authoritative_profile()));
			}
			if (envelope.has_world_object_rejected())
			{
				const auto &message = envelope.world_object_rejected();
				return message.has_authoritative_state()
				    && is_valid_world_object_state(
				        message.authoritative_state())
				    && valid_utf8_with_codepoint_count(
				        message.reason(), 1, 512);
			}
			if (envelope.has_world_object_updated())
			{
				return envelope.world_object_updated().has_state()
				    && is_valid_world_object_state(
				        envelope.world_object_updated().state());
			}
			if (envelope.has_client_world_item_update())
			{
				const auto &message = envelope.client_world_item_update();
				return message.has_state()
				    && is_valid_world_item_state(message.state(), false)
				    && message.state().revision() == message.base_revision()
				    && (message.source_instance_id().empty()
				        || is_uuid(message.source_instance_id()))
				    && message.transfer_count() <= max_profile_item_count;
			}
			if (envelope.has_world_item_accepted())
			{
				const auto &message = envelope.world_item_accepted();
				return is_uuid(message.instance_id()) && message.revision() > 0
				    && (!message.has_authoritative_profile()
				        || is_valid_profile(message.authoritative_profile()));
			}
			if (envelope.has_world_item_rejected())
			{
				const auto &message = envelope.world_item_rejected();
				return message.has_authoritative_state()
				    && is_valid_world_item_state(
				        message.authoritative_state())
				    && valid_utf8_with_codepoint_count(
				        message.reason(), 1, 512);
			}
			if (envelope.has_world_item_updated())
			{
				return envelope.world_item_updated().has_state()
				    && is_valid_world_item_state(
				        envelope.world_item_updated().state());
			}
			if (envelope.has_server_environment_updated())
			{
				const auto &message = envelope.server_environment_updated();
				return message.has_state()
				    && is_valid_environment_state(message.state());
			}
			if (envelope.has_server_sleep_state())
			{
				const auto &message = envelope.server_sleep_state();
				return message.revision() > 0
				    && message.required_players() > 0
				    && message.required_players() <= max_players
				    && message.sleeping_players() <= max_players;
			}
			if (envelope.has_server_respawn())
			{
				return envelope.server_respawn().has_spawn()
				    && is_finite_transform(envelope.server_respawn().spawn());
			}
			if (envelope.has_client_activity_start())
			{
				const auto &message = envelope.client_activity_start();
				return protocol::PlayerActivityKind_IsValid(
				           static_cast<int>(message.kind()))
				    && message.kind() != protocol::PLAYER_ACTIVITY_KIND_NONE
				    && message.station_guid() != 0;
			}
			if (envelope.has_client_activity_end())
			{
				const auto &message = envelope.client_activity_end();
				return message.session_id() != 0
				    && (!message.has_final_transform()
				        || is_finite_transform(message.final_transform()));
			}
			if (envelope.has_activity_granted())
			{
				const auto &message = envelope.activity_granted();
				return message.has_activity() && message.activity().active()
				    && message.activity().kind()
				        != protocol::PLAYER_ACTIVITY_KIND_NONE
				    && protocol::PlayerActivityKind_IsValid(
				        static_cast<int>(message.activity().kind()))
				    && message.activity().station_guid() != 0
				    && message.activity().session_id() != 0
				    && message.activity().revision() != 0;
			}
			if (envelope.has_activity_denied())
			{
				const auto &message = envelope.activity_denied();
				return message.kind() != protocol::PLAYER_ACTIVITY_KIND_NONE
				    && protocol::PlayerActivityKind_IsValid(
				        static_cast<int>(message.kind()))
				    && message.station_guid() != 0
				    && valid_utf8_with_codepoint_count(message.reason(), 1, 256);
			}
			if (envelope.has_player_activity_updated())
			{
				const auto &message = envelope.player_activity_updated();
				if (message.player_id() == 0 || !message.has_activity()
				    || message.activity().kind()
				        == protocol::PLAYER_ACTIVITY_KIND_NONE
				    || !protocol::PlayerActivityKind_IsValid(
				        static_cast<int>(message.activity().kind()))
				    || message.activity().station_guid() == 0
				    || message.activity().session_id() == 0
				    || message.activity().revision() == 0)
				{
					return false;
				}
				return !message.has_final_transform()
				    || is_finite_transform(message.final_transform());
			}
			if (envelope.has_client_avatar_update())
			{
				const auto &message = envelope.client_avatar_update();
				return message.base_revision() > 0 && message.has_avatar()
				    && message.avatar().revision() == message.base_revision();
			}
			if (envelope.has_avatar_accepted())
			{
				return envelope.avatar_accepted().revision() > 0;
			}
			if (envelope.has_avatar_rejected())
			{
				const auto &message = envelope.avatar_rejected();
				return message.has_authoritative_avatar()
				    && is_valid_avatar_descriptor(
				        message.authoritative_avatar())
				    && valid_utf8_with_codepoint_count(
				        message.reason(), 1, 512);
			}
			if (envelope.has_player_avatar_updated())
			{
				const auto &message = envelope.player_avatar_updated();
				return message.player_id() != 0 && message.has_avatar()
				    && is_valid_avatar_descriptor(message.avatar());
			}
			if (envelope.has_server_accepted())
			{
				const auto &message = envelope.server_accepted();
				if (message.players_size() > static_cast<int>(max_players)
				    || message.profile_snapshot_interval_seconds() < 5
				    || message.profile_snapshot_interval_seconds() > 60
				    || !message.has_avatar_policy()
				    || !is_valid_avatar_policy(message.avatar_policy())
				    || (message.has_home_marker()
				        && !valid_home_marker(message.home_marker())))
				{
					return false;
				}
				for (const auto &player : message.players())
				{
					if (!valid_player_snapshot(player, true))
					{
						return false;
					}
				}
			}
			else if (envelope.has_server_home_marker_updated())
			{
				const auto &message = envelope.server_home_marker_updated();
				return message.ledger_revision() != 0
				    && (!message.active()
				        || (message.has_marker()
				            && valid_home_marker(message.marker())));
			}
			else if (envelope.has_server_rejected())
			{
				return protocol::RejectReason_IsValid(
				    static_cast<int>(envelope.server_rejected().reason()));
			}
			else if (envelope.has_player_joined())
			{
				return valid_player_snapshot(
				    envelope.player_joined().player(),
				    true);
			}
			else if (envelope.has_client_transform())
			{
				return envelope.client_transform().has_transform()
				    && is_finite_transform(
				        envelope.client_transform().transform());
			}
			else if (envelope.has_world_snapshot())
			{
				const auto &message = envelope.world_snapshot();
				if (message.players_size() > static_cast<int>(max_players)
				    || !message.has_environment()
				    || !is_valid_environment_state(message.environment()))
				{
					return false;
				}
				for (const auto &player : message.players())
				{
					if (!valid_player_snapshot(player, false)
					    || player.has_avatar())
					{
						return false;
					}
				}
			}
			else if (envelope.has_state_correction())
			{
				return envelope.state_correction().has_accepted_transform()
				    && is_finite_transform(
				        envelope.state_correction().accepted_transform());
			}
			else if (envelope.has_chat_send())
			{
				return is_valid_chat(envelope.chat_send().text());
			}
			else if (envelope.has_chat_broadcast())
			{
				return is_valid_display_name(
				           envelope.chat_broadcast().display_name())
				    && is_valid_chat(envelope.chat_broadcast().text());
			}
			return true;
		}
	}

	traffic_lane lane_for(const protocol::Envelope &envelope) noexcept
	{
		switch (envelope.payload_case())
		{
		case protocol::Envelope::kChatSend:
		case protocol::Envelope::kChatBroadcast:
			return traffic_lane::interactive;

		case protocol::Envelope::kClientTransform:
		case protocol::Envelope::kWorldSnapshot:
			return traffic_lane::player_realtime;

		case protocol::Envelope::kClientNpcUpdate:
		case protocol::Envelope::kClientNpcUpdateBatch:
		case protocol::Envelope::kServerNpcSnapshot:
		case protocol::Envelope::kServerNpcMotion:
			return traffic_lane::npc_realtime;

		default:
			return traffic_lane::ordered_state;
		}
	}

	std::optional<encoded_message> encode(
	    const protocol::Envelope &envelope,
	    reliability delivery,
	    std::string *error)
	{
		if (envelope.payload_case() == protocol::Envelope::PAYLOAD_NOT_SET)
		{
			set_error(error, "envelope has no payload");
			return std::nullopt;
		}
		if (!valid_envelope(envelope))
		{
			set_error(error, "envelope violates protocol limits");
			return std::nullopt;
		}

		const auto size = envelope.ByteSizeLong();
		if (size == 0 || size > max_application_message_size
		    || size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		{
			set_error(error, "encoded message exceeds the application limit");
			return std::nullopt;
		}

		encoded_message result;
		result.delivery = delivery;
		result.bytes.resize(size);
		if (!envelope.SerializeToArray(result.bytes.data(), static_cast<int>(result.bytes.size())))
		{
			set_error(error, "protobuf serialization failed");
			return std::nullopt;
		}
		return result;
	}

	bool is_valid_npc_kind(protocol::NpcKind kind)
	{
		return kind == protocol::NPC_KIND_HUMAN
		    || kind == protocol::NPC_KIND_ANIMAL;
	}

	bool is_valid_npc_observation(
	    const protocol::NpcObservation &observation)
	{
		return observation.authored_guid() != 0
		    && is_valid_npc_kind(observation.kind())
		    && observation.has_transform()
		    && is_finite_transform(observation.transform())
		    && (!observation.has_gameplay()
		        || valid_npc_gameplay(observation.gameplay()))
		    && (!observation.dynamic()
		        || (valid_utf8_with_codepoint_count(
		                observation.entity_class(), 1, 64)
		            && valid_utf8_with_codepoint_count(
		                observation.entity_name(), 1, 128)));
	}

	bool is_valid_npc_state(const protocol::NpcState &state)
	{
		return state.npc_id() != 0 && state.generation() != 0
		    && state.authored_guid() != 0 && is_valid_npc_kind(state.kind())
		    && state.has_transform() && is_finite_transform(state.transform())
		    && state.revision() != 0
		    && (!state.has_gameplay() || valid_npc_gameplay(state.gameplay()))
		    && (!state.dynamic()
		        || (valid_utf8_with_codepoint_count(state.entity_class(), 1, 64)
		            && valid_utf8_with_codepoint_count(state.entity_name(), 1, 128)))
		    && ((state.authority_player_id() == 0 && state.lease_id() == 0)
		        || (state.authority_player_id() != 0 && state.lease_id() != 0));
	}

	std::optional<protocol::Envelope> decode(
	    std::span<const std::byte> bytes,
	    std::string *error)
	{
		if (bytes.empty() || bytes.size() > max_application_message_size
		    || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		{
			set_error(error, "message size is invalid");
			return std::nullopt;
		}

		protocol::Envelope envelope;
		if (!envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))
		    || envelope.payload_case() == protocol::Envelope::PAYLOAD_NOT_SET)
		{
			set_error(error, "protobuf payload is malformed or empty");
			return std::nullopt;
		}
		if (!valid_envelope(envelope))
		{
			set_error(error, "envelope violates protocol limits");
			return std::nullopt;
		}
		return envelope;
	}

	bool valid_utf8_with_codepoint_count(
	    std::string_view value,
	    std::size_t minimum,
	    std::size_t maximum)
	{
		std::size_t codepoints = 0;
		for (std::size_t index = 0; index < value.size();)
		{
			const auto lead = static_cast<unsigned char>(value[index]);
			std::size_t length = 0;
			std::uint32_t codepoint = 0;
			if (lead <= 0x7F)
			{
				length = 1;
				codepoint = lead;
			}
			else if ((lead & 0xE0) == 0xC0)
			{
				length = 2;
				codepoint = lead & 0x1F;
			}
			else if ((lead & 0xF0) == 0xE0)
			{
				length = 3;
				codepoint = lead & 0x0F;
			}
			else if ((lead & 0xF8) == 0xF0)
			{
				length = 4;
				codepoint = lead & 0x07;
			}
			else
			{
				return false;
			}

			if (index + length > value.size())
			{
				return false;
			}
			for (std::size_t continuation = 1; continuation < length; ++continuation)
			{
				const auto byte = static_cast<unsigned char>(value[index + continuation]);
				if ((byte & 0xC0) != 0x80)
				{
					return false;
				}
				codepoint = (codepoint << 6) | (byte & 0x3F);
			}

			const bool overlong = (length == 2 && codepoint < 0x80)
			    || (length == 3 && codepoint < 0x800)
			    || (length == 4 && codepoint < 0x10000);
			if (overlong || codepoint > 0x10FFFF
			    || (codepoint >= 0xD800 && codepoint <= 0xDFFF)
			    || codepoint == 0)
			{
				return false;
			}
			++codepoints;
			if (codepoints > maximum)
			{
				return false;
			}
			index += length;
		}
		return codepoints >= minimum && codepoints <= maximum;
	}

	bool is_valid_display_name(std::string_view value)
	{
		if (!valid_utf8_with_codepoint_count(
		        value,
		        min_display_name_codepoints,
		        max_display_name_codepoints))
		{
			return false;
		}
		return value.front() != ' ' && value.back() != ' ';
	}

	bool is_valid_chat(std::string_view value)
	{
		return valid_utf8_with_codepoint_count(value, 1, max_chat_codepoints);
	}

	bool is_uuid(std::string_view value)
	{
		if (value.size() != 36)
			return false;
		for (std::size_t index = 0; index < value.size(); ++index)
		{
			if (index == 8 || index == 13 || index == 18 || index == 23)
			{
				if (value[index] != '-')
					return false;
				continue;
			}
			const auto character =
			    static_cast<unsigned char>(value[index]);
			if (!std::isxdigit(character))
				return false;
		}
		return true;
	}

	bool is_valid_avatar_equipment_slot(std::string_view value)
	{
		if (value.empty() || value.size() > 64)
			return false;
		std::string lowered(value);
		std::ranges::transform(
		    lowered,
		    lowered.begin(),
		    [](unsigned char character)
		    {
			    return static_cast<char>(std::tolower(character));
		    });
		if (lowered.starts_with("horse_")
		    || lowered.starts_with("cattle_"))
		{
			return false;
		}
		return std::ranges::all_of(
		    value,
		    [](unsigned char character)
		    {
			    return std::isalnum(character) || character == '_';
		    });
	}

	bool is_valid_avatar_descriptor(
	    const protocol::AvatarDescriptor &avatar)
	{
		if (!is_uuid(avatar.archetype_id())
		    || avatar.revision() == 0
		    || avatar.equipment_size()
		        > static_cast<int>(max_avatar_equipment_items)
		    || !protocol::AvatarStance_IsValid(
		        static_cast<int>(avatar.stance()))
		    || !protocol::AvatarWeaponClass_IsValid(
		        static_cast<int>(avatar.weapon_class()))
		    || !protocol::AvatarWeaponSet_IsValid(
		        static_cast<int>(avatar.active_weapon_set()))
		    || (avatar.weapon_drawn()
		        && avatar.weapon_class()
		            == protocol::AVATAR_WEAPON_CLASS_NONE)
		    || (avatar.weapon_drawn()
		        && avatar.active_weapon_set()
		            == protocol::AVATAR_WEAPON_SET_NONE)
		    || (!avatar.weapon_drawn()
		        && avatar.active_weapon_set()
		            != protocol::AVATAR_WEAPON_SET_NONE)
		    || (avatar.weapon_class()
		            == protocol::AVATAR_WEAPON_CLASS_UNARMED
		        && (!avatar.weapon_drawn()
		            || avatar.active_weapon_set()
		                != protocol::AVATAR_WEAPON_SET_PRIMARY)))
		{
			return false;
		}
		std::unordered_set<std::string> slots;
		return std::ranges::all_of(
		    avatar.equipment(),
		    [&](const protocol::AvatarEquipment &item)
		    {
			    return is_uuid(item.definition_id())
			        && is_valid_avatar_equipment_slot(
			            item.equipped_slot())
			        && slots.insert(item.equipped_slot()).second;
		    });
	}

	bool is_valid_avatar_policy(const protocol::AvatarPolicy &policy)
	{
		if (!is_uuid(policy.default_archetype_id())
		    || policy.allowed_archetype_ids_size() == 0
		    || policy.allowed_archetype_ids_size()
		        > static_cast<int>(max_avatar_archetypes))
		{
			return false;
		}
		bool contains_default = false;
		std::unordered_set<std::string> archetypes;
		for (const auto &archetype : policy.allowed_archetype_ids())
		{
			if (!is_uuid(archetype)
			    || !archetypes.insert(archetype).second)
			{
				return false;
			}
			contains_default =
			    contains_default || archetype == policy.default_archetype_id();
		}
		return contains_default;
	}

	bool is_valid_address_library_identity(
	    const protocol::ClientRuntimeCapabilities &runtime)
	{
		return valid_identifier(runtime.address_library(), 64)
		    && (runtime.address_library_distribution() == "steam"
		        || runtime.address_library_distribution() == "gog"
		        || runtime.address_library_distribution() == "epic")
		    && runtime.address_library_format() != 0
		    && runtime.address_library_entries() != 0
		    && runtime.address_library_sha256().size() == 64
		    && std::ranges::all_of(
		        runtime.address_library_sha256(),
		        [](unsigned char value)
		        {
			        return (value >= '0' && value <= '9')
			            || (value >= 'a' && value <= 'f');
		        });
	}

	bool is_supported_address_library_identity(
	    const protocol::ClientRuntimeCapabilities &runtime)
	{
		return is_valid_address_library_identity(runtime)
		    && std::ranges::any_of(
		        supported_address_libraries,
		        [&](const address_library_identity &identity)
		        {
			        return runtime.address_library_distribution()
			                == identity.distribution
			            && runtime.address_library() == identity.build_key
			            && runtime.address_library_format()
			                == identity.format_version
			            && runtime.address_library_entries()
			                == identity.entry_count
			            && runtime.address_library_sha256() == identity.sha256;
		        });
	}

	bool is_valid_profile(const protocol::PlayerProfile &profile)
	{
		if (profile.player_id() == 0 || profile.revision() == 0
		    || !is_valid_display_name(profile.display_name())
		    || (!profile.persistent_id().empty()
		        && !is_uuid(profile.persistent_id()))
		    || !valid_identifier(profile.level_id())
		    || profile.money() < 0 || profile.money() > max_profile_money
		    || profile.money_subunits() >= money_subunits_per_groschen
		    || profile.stats_size() != static_cast<int>(profile_stat_count)
		    || profile.skills_size() != static_cast<int>(profile_skill_count)
		    || profile.inventory_size()
		        > static_cast<int>(max_profile_inventory_items)
		    || (profile.transform_valid()
		        && (!profile.has_last_transform()
		            || !is_finite_transform(profile.last_transform())))
		    || !profile.has_avatar()
		    || !is_valid_avatar_descriptor(profile.avatar()))
		{
			return false;
		}
		const auto valid_rpg = [](const protocol::RpgValue &value)
		{
			return value.level() >= 0 && value.level() <= 100
			    && std::isfinite(value.progress())
			    && value.progress() >= 0.0F && value.progress() <= 1.0F;
		};
		const auto exact_rpg_set = [&](const auto &values, const auto &ids)
		{
			std::unordered_set<std::string_view> found;
			for (const auto &value : values)
			{
				if (!valid_rpg(value)
				    || std::ranges::find(ids, value.id()) == ids.end()
				    || !found.insert(value.id()).second)
				{
					return false;
				}
			}
			return found.size() == ids.size();
		};
		if (!exact_rpg_set(profile.stats(), canonical_stat_ids)
		    || !exact_rpg_set(profile.skills(), canonical_skill_ids))
		{
			return false;
		}

		std::unordered_set<std::string> instance_ids;
		std::unordered_set<std::string> equipped_slots;
		for (const auto &item : profile.inventory())
		{
			if (!valid_inventory_item(item, true)
			    || !instance_ids.insert(item.instance_id()).second)
			{
				return false;
			}
			if (item.has_equipped_slot()
			    && (!is_valid_avatar_equipment_slot(item.equipped_slot())
			        || !equipped_slots.insert(item.equipped_slot()).second))
			{
				return false;
			}
		}
		if (profile.quick_access_slots_size()
		        > static_cast<int>(max_profile_quick_access_slots))
		{
			return false;
		}
		std::unordered_set<std::uint32_t> quick_slots;
		for (const auto &slot : profile.quick_access_slots())
		{
			if (slot.outfit() > 2
			    || !protocol::QuickAccessSlotType_IsValid(
			        static_cast<int>(slot.type()))
			    || (slot.type() == protocol::QUICK_ACCESS_SLOT_TYPE_WEAPON
			            ? slot.slot() > 7
			            : slot.slot() > 3)
			    || !instance_ids.contains(slot.instance_id()))
			{
				return false;
			}
			const auto key = slot.outfit() * 16U
			    + (slot.type() == protocol::QUICK_ACCESS_SLOT_TYPE_CONSUMABLE
			            ? 8U
			            : 0U)
			    + slot.slot();
			if (!quick_slots.insert(key).second)
				return false;
		}
		for (const auto &visible : profile.avatar().equipment())
		{
			const auto match = std::ranges::find_if(
			    profile.inventory(),
			    [&](const protocol::InventoryItem &item)
			    {
				    return item.definition_id() == visible.definition_id()
				        && item.has_equipped_slot()
				        && item.equipped_slot() == visible.equipped_slot();
			    });
			if (match == profile.inventory().end())
				return false;
		}
		return true;
	}

	bool is_valid_world_object_state(
	    const protocol::WorldObjectState &state,
	    bool require_revision)
	{
		if (state.entity_guid() == 0
		    || !protocol::WorldObjectKind_IsValid(
		        static_cast<int>(state.kind()))
		    || state.kind() == protocol::WORLD_OBJECT_KIND_UNSPECIFIED
		    || (require_revision && state.revision() == 0)
		    || state.inventory_size()
		        > static_cast<int>(max_world_object_inventory_items)
		    || (state.kind() == protocol::WORLD_OBJECT_KIND_DOOR
		        && (state.has_inventory() || state.inventory_size() != 0))
		    || (!state.has_inventory() && state.inventory_size() != 0))
		{
			return false;
		}
		std::unordered_set<std::string> instances;
		return std::ranges::all_of(
		    state.inventory(),
		    [&](const protocol::InventoryItem &item)
		    {
			    return valid_inventory_item(item, false)
			        && instances.insert(item.instance_id()).second;
		    });
	}

	bool is_valid_world_item_state(
	    const protocol::WorldItemState &state,
	    bool require_revision)
	{
		auto transform = state.transform();
		return is_uuid(state.instance_id())
		    && (!require_revision || state.revision() != 0)
		    && state.has_item()
		    && state.item().instance_id() == state.instance_id()
		    && valid_inventory_item(state.item(), false)
		    && state.has_transform()
		    && is_finite_transform(state.transform())
		    && normalize_rotation(transform.mutable_rotation());
	}

	bool is_finite_transform(const protocol::TransformState &transform)
	{
		if (!transform.has_position() || !transform.has_rotation() || !transform.has_velocity())
		{
			return false;
		}
		const auto &position = transform.position();
		const auto &rotation = transform.rotation();
		const auto &velocity = transform.velocity();
		const bool base_finite = finite(position.x()) && finite(position.y()) && finite(position.z())
		    && finite(rotation.x()) && finite(rotation.y()) && finite(rotation.z())
		    && finite(rotation.w()) && finite(velocity.x()) && finite(velocity.y())
		    && finite(velocity.z());
		if (!base_finite)
			return false;

		if (transform.has_locomotion())
		{
			const auto &state = transform.locomotion();
			if (!state.has_local_velocity() || !state.has_acceleration()
			    || !state.has_facing_direction())
				return false;
			auto finite_vec = [](const protocol::Vec3 &value)
			{
				return finite(value.x()) && finite(value.y()) && finite(value.z());
			};
			if (!finite_vec(state.local_velocity())
			    || !finite_vec(state.acceleration())
			    || !finite_vec(state.facing_direction())
			    || !finite(state.speed()) || state.speed() < 0.0F
			    || state.speed() > 100.0F
			    || !finite(state.yaw_rate())
			    || std::abs(state.yaw_rate()) > 100.0F)
				return false;
		}

		if (transform.has_animation())
		{
			const auto &animation = transform.animation();
			if (animation.sequence() == 0
			    || animation.fragment().size() > max_animation_fragment_bytes
			    || (animation.active()
			        && !is_non_combat_animation_fragment(animation.fragment()))
			    || (!animation.active() && !animation.fragment().empty())
			    || !std::ranges::all_of(
			        animation.fragment(),
			        [](unsigned char character)
			        {
				        return std::isalnum(character) != 0
				            || character == '_' || character == '-'
				            || character == '/' || character == '.'
				            || character == ':';
			        }))
				return false;
		}
		return true;
	}

	bool normalize_rotation(protocol::Quaternion *rotation)
	{
		if (!rotation)
		{
			return false;
		}
		const auto length_squared = rotation->x() * rotation->x()
		    + rotation->y() * rotation->y()
		    + rotation->z() * rotation->z()
		    + rotation->w() * rotation->w();
		if (!std::isfinite(length_squared) || length_squared < 0.000001F)
		{
			return false;
		}
		const auto inverse_length = 1.0F / std::sqrt(length_squared);
		rotation->set_x(rotation->x() * inverse_length);
		rotation->set_y(rotation->y() * inverse_length);
		rotation->set_z(rotation->z() * inverse_length);
		rotation->set_w(rotation->w() * inverse_length);
		return true;
	}

	protocol::MovementMode movement_mode_for(
	    const protocol::TransformState &transform)
	{
		if (!transform.has_velocity())
		{
			return protocol::MOVEMENT_MODE_IDLE;
		}
		const auto horizontal_speed = std::hypot(
		    transform.velocity().x(),
		    transform.velocity().y());
		if (horizontal_speed < 0.15F)
		{
			return protocol::MOVEMENT_MODE_IDLE;
		}
		if (horizontal_speed < 3.2F)
		{
			return protocol::MOVEMENT_MODE_WALK;
		}
		if (horizontal_speed < 4.3F)
			return protocol::MOVEMENT_MODE_RUN;
		return protocol::MOVEMENT_MODE_SPRINT;
	}
}
