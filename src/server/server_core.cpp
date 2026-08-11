#include "server/server_core.hpp"

#include "property/catalog.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace kcd2o::server
{
	namespace
	{
		std::string lowercase_ascii(std::string_view value)
		{
			std::string result(value);
			std::ranges::transform(
			    result,
			    result.begin(),
			    [](unsigned char character)
			    {
				    return static_cast<char>(std::tolower(character));
			    });
			return result;
		}

		protocol::Envelope player_left_envelope(
		    player_id id,
		    const std::string &reason)
		{
			protocol::Envelope envelope;
			auto *left = envelope.mutable_player_left();
			left->set_player_id(id);
			left->set_reason(reason);
			return envelope;
		}

		bool profile_name_matches(
		    const protocol::PlayerProfile &profile,
		    std::string_view display_name)
		{
			return lowercase_ascii(profile.display_name())
			    == lowercase_ascii(display_name);
		}

		server_config validated_config(server_config config)
		{
			normalize_avatar_config(config);
			validate_server_config(config);
			return config;
		}

		std::uint64_t unix_milliseconds()
		{
			return static_cast<std::uint64_t>(
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        std::chrono::system_clock::now().time_since_epoch())
			        .count());
		}

		protocol::NetworkRole network_role(std::string_view role)
		{
			if (role == "supporter") return protocol::NETWORK_ROLE_SUPPORTER;
			if (role == "moderator") return protocol::NETWORK_ROLE_MODERATOR;
			if (role == "admin") return protocol::NETWORK_ROLE_ADMIN;
			if (role == "owner") return protocol::NETWORK_ROLE_OWNER;
			return protocol::NETWORK_ROLE_USER;
		}
	}

	server_core::server_core(
	    server_config config,
	    token_generator generate_token,
	    account_authenticator authenticate_account) :
	    m_config(validated_config(std::move(config))),
	    m_generate_token(generate_token ? std::move(generate_token) : []
	        { return random_hex(32); }),
	    m_authenticate_account(std::move(authenticate_account)),
	    m_store(m_config),
	    m_permissions(m_config.world_directory, m_config.permission_owners),
	    m_npcs(m_store.manifest().level_id, m_config.npc_world_catalog_path),
	    m_human_npcs_disabled(m_config.disable_human_npcs),
	    m_animal_npcs_disabled(m_config.disable_animal_npcs),
	    m_environment_anchor_world_seconds(
	        m_config.initial_time_of_day_hours * seconds_per_hour),
	    m_environment_time_scale(m_config.time_scale),
	    m_environment_weather_id(m_config.weather_id),
	    m_environment_weather_transition_ms(
	        m_config.weather_transition_seconds * 1000U)
	{
		if (m_human_npcs_disabled)
			(void)m_npcs.disable_kind(protocol::NPC_KIND_HUMAN);
		if (m_animal_npcs_disabled)
			(void)m_npcs.disable_kind(protocol::NPC_KIND_ANIMAL);
		const auto catalog_needs_markers = std::ranges::none_of(
		    m_store.property_catalog().properties(),
		    [](const protocol::PropertyDefinition &definition)
		    {
			    return definition.has_marker_position()
			        && definition.marker_entity_guid() != 0;
		    });
		if ((m_store.property_catalog().properties().empty()
		        || catalog_needs_markers)
		    && !m_config.property_game_data.empty())
		{
			protocol::PropertyCatalog catalog;
			const auto path = m_config.property_game_data
			    / ("property_catalog_" + m_store.manifest().level_id + ".pb");
			std::ifstream input(path, std::ios::binary);
			if (!input || !catalog.ParseFromIstream(&input))
			{
				throw std::runtime_error(
				    "could not load generated property catalog: "
				    + path.string());
			}
			try
			{
				m_store.save_property_catalog(catalog);
			}
			catch (const std::invalid_argument &)
			{
				throw std::runtime_error(
				    "generated property catalog does not match level "
				    + m_store.manifest().level_id + ": " + path.string());
			}
		}
		m_properties.reset(
		    m_store.property_catalog(), m_store.property_ledger());
		for (const auto &object : m_store.world_objects())
			m_world_objects.emplace(object.entity_guid(), object);
		for (const auto &item : m_store.world_items())
			m_world_items.emplace(item.instance_id(), item);
		std::string item_error;
		for (const auto &stored : m_store.profiles())
		{
			for (const auto &item : stored.profile.inventory())
			{
				if (!m_items.register_item(
				        item,
				        item_location::player(stored.profile.player_id()),
				        item_error))
				{
					throw std::runtime_error(
					    "invalid persisted item ownership: " + item_error);
				}
			}
		}
		remove_owned_items_from_world();
		for (const auto &[guid, object] : m_world_objects)
		{
			for (const auto &item : object.inventory())
			{
				if (!m_items.register_item(
				        item, item_location::container(guid), item_error))
				{
					throw std::runtime_error(
					    "invalid persisted container item: " + item_error);
				}
			}
		}
		for (const auto &[instance, item] : m_world_items)
		{
			(void)instance;
			if (item.present()
			    && !m_items.register_item(
			        item.item(), item_location::world(), item_error))
			{
				throw std::runtime_error(
				    "invalid persisted world item: " + item_error);
			}
		}
	}

	void server_core::on_transport_connected(
	    connection_id connection,
	    time_point now)
	{
		advance_environment_clock(now);
		if (connection == 0 || m_pending.contains(connection)
		    || find_by_connection(connection))
		{
			return;
		}
		m_pending.emplace(
		    connection,
		    pending_connection{
		        .connected_at = now,
		        .deadline =
		            now + std::chrono::seconds(
		                m_config.handshake_timeout_seconds)});
	}

	void server_core::on_transport_disconnected(
	    connection_id connection,
	    bool allow_reconnect,
	    std::string reason,
	    time_point now)
	{
		advance_environment_clock(now);
		if (m_pending.contains(connection))
		{
			release_initializer(connection);
			m_pending.erase(connection);
			wake_bootstrap_waiters();
		}
		auto *player = find_by_connection(connection);
		if (!player)
		{
			return;
		}
		persist_player(*player, now);
		release_activity(*player);
		if (allow_reconnect)
		{
			remove_sleep_vote(player->id);
			broadcast_system_message(
			    player->display_name
			        + " lost connection; waiting for reconnection.",
			    now,
			    connection);
			player->connection.reset();
			const auto positions = player_positions();
			queue_npc_events(m_npcs.remove_player(player->id, positions, now));
			player->reconnect_deadline =
			    now + std::chrono::seconds(m_config.reconnect_grace_seconds);
			return;
		}
		remove_player(player->id, std::move(reason), close_kind::none, now);
	}

	void server_core::on_message(
	    connection_id connection,
	    const protocol::Envelope &envelope,
	    time_point now)
	{
		advance_environment_clock(now);
		if (auto *player = find_by_connection(connection))
		{
			player->last_message_at = now;
			switch (envelope.payload_case())
			{
			case protocol::Envelope::kClientTransform:
				handle_transform(*player, envelope.client_transform(), now);
				break;
			case protocol::Envelope::kChatSend:
				handle_chat(*player, envelope.chat_send(), now);
				break;
			case protocol::Envelope::kClientProfileUpdate:
				handle_profile_update(
				    *player,
				    envelope.client_profile_update(),
				    now);
				break;
			case protocol::Envelope::kClientAvatarUpdate:
				handle_avatar_update(
				    *player,
				    envelope.client_avatar_update(),
				    now);
				break;
			case protocol::Envelope::kClientWorldObjectUpdate:
				handle_world_object_update(
				    *player,
				    envelope.client_world_object_update());
				break;
			case protocol::Envelope::kClientWorldItemUpdate:
				handle_world_item_update(
				    *player,
				    envelope.client_world_item_update());
				break;
			case protocol::Envelope::kClientNpcDiscovery:
				handle_npc_discovery(
				    *player,
				    envelope.client_npc_discovery(),
				    now);
				break;
			case protocol::Envelope::kClientNpcUpdate:
				handle_npc_update(*player, envelope.client_npc_update(), now);
				break;
			case protocol::Envelope::kClientNpcUpdateBatch:
				for (const auto &update :
				     envelope.client_npc_update_batch().updates())
					handle_npc_update(*player, update, now);
				break;
			case protocol::Envelope::kPing:
				handle_ping(*player, envelope.ping(), now);
				break;
			case protocol::Envelope::kClientSleepState:
				handle_sleep_state(
				    *player,
				    envelope.client_sleep_state(),
				    now);
				break;
			case protocol::Envelope::kClientDeath:
				handle_death(*player, now);
				break;
			case protocol::Envelope::kClientRespawnRequest:
				handle_respawn_request(*player, now);
				break;
			case protocol::Envelope::kClientActivityStart:
				handle_activity_start(
				    *player,
				    envelope.client_activity_start());
				break;
			case protocol::Envelope::kClientActivityEnd:
				handle_activity_end(
				    *player,
				    envelope.client_activity_end());
				break;
			case protocol::Envelope::kClientVoiceFrame:
				handle_voice(*player, envelope.client_voice_frame(), now);
				break;
			default:
				reject(
				    connection,
				    protocol::REJECT_REASON_MALFORMED_MESSAGE,
				    "message is not valid in the connected state");
				break;
			}
			return;
		}

		auto iterator = m_pending.find(connection);
		if (iterator == m_pending.end())
		{
			reject(
			    connection,
			    protocol::REJECT_REASON_MALFORMED_MESSAGE,
			    "ClientHello must be the first message");
			return;
		}
		auto &pending = iterator->second;
		if (envelope.has_ping() && pending.stage != pending_stage::hello)
		{
			protocol::Envelope pong;
			pong.mutable_pong()->set_nonce(envelope.ping().nonce());
			pong.mutable_pong()->set_client_time_ms(
			    envelope.ping().client_time_ms());
			pong.mutable_pong()->set_server_time_ms(milliseconds(now));
			queue(connection, std::move(pong), reliability::reliable);
			return;
		}
		switch (pending.stage)
		{
		case pending_stage::hello:
			if (!envelope.has_client_hello())
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_MALFORMED_MESSAGE,
				    "ClientHello must be the first message");
			}
			else
			{
				handle_hello(connection, envelope.client_hello(), now);
			}
			break;
		case pending_stage::authenticate:
			if (!envelope.has_client_authenticate())
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_MALFORMED_MESSAGE,
				    "ClientAuthenticate must follow ServerChallenge");
			}
			else
			{
				handle_authenticate(
				    connection,
				    envelope.client_authenticate(),
				    now);
			}
			break;
		case pending_stage::waiting_for_initializer:
		case pending_stage::loading_world:
			if (envelope.has_client_world_ready())
			{
				handle_world_ready(
				    connection,
				    envelope.client_world_ready(),
				    now);
			}
			else if (envelope.has_client_world_failed())
			{
				handle_world_failed(
				    connection,
				    envelope.client_world_failed(),
				    now);
			}
			else
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_MALFORMED_MESSAGE,
				    "client is not ready for gameplay messages");
			}
			break;
		}
	}

	void server_core::tick(time_point now)
	{
		advance_environment_clock(now);
		++m_server_tick;
		tick_dummies(now);
		std::vector<connection_id> expired_pending;
		for (const auto &[connection, pending] : m_pending)
		{
			const auto handshake_pending =
			    pending.stage == pending_stage::hello
			    || pending.stage == pending_stage::authenticate;
			if (handshake_pending && now >= pending.deadline)
			{
				expired_pending.push_back(connection);
			}
		}
		for (const auto connection : expired_pending)
		{
			reject(
			    connection,
			    protocol::REJECT_REASON_BOOTSTRAP_FAILED,
			    "handshake timed out");
		}

		std::vector<player_id> expired_players;
		for (auto &[id, player] : m_players)
		{
			if (player.dummy)
			{
				continue;
			}
			if (player.has_transform
			    && (player.last_persisted_at == time_point{}
			        || now - player.last_persisted_at >= std::chrono::seconds(5)))
			{
				persist_player(player, now);
			}
			if (!player.connection && now >= player.reconnect_deadline)
			{
				expired_players.push_back(id);
			}
			else if (player.connection
			    && now - player.last_message_at
			        >= std::chrono::seconds(m_config.idle_timeout_seconds))
			{
				expired_players.push_back(id);
			}
		}
		for (const auto id : expired_players)
		{
			remove_player(id, "timed out", close_kind::kick, now);
		}

		for (auto iterator = m_claims.begin(); iterator != m_claims.end();)
		{
			iterator = now >= iterator->second.expires_at
			    ? m_claims.erase(iterator)
			    : std::next(iterator);
		}

		const auto npc_positions = player_positions();
		queue_npc_events(m_npcs.reconcile(npc_positions, now));

		const auto snapshot_interval =
		    std::chrono::duration<double>(1.0 / m_config.snapshot_rate);
		if (m_last_snapshot == time_point{}
		    || now - m_last_snapshot >= snapshot_interval)
		{
			queue_snapshot(now);
			m_last_snapshot = now;
		}
	}

	void server_core::kick(player_id id, std::string reason, time_point now)
	{
		remove_player(id, std::move(reason), close_kind::kick, now);
	}

	std::optional<player_id> server_core::spawn_dummy(
	    std::string display_name,
	    std::string *error)
	{
		if (error)
		{
			error->clear();
		}
		const auto set_error = [&](std::string message)
		{
			if (error)
			{
				*error = std::move(message);
			}
		};
		const auto reserved_slots =
		    m_players.size()
		    + static_cast<std::size_t>(std::ranges::count_if(
		        m_pending,
		        [](const auto &entry)
		        {
			        return entry.second.persisted.has_value();
		        }));
		if (reserved_slots >= m_config.max_players)
		{
			set_error("server is full");
			return std::nullopt;
		}

		const auto name_in_use = [&](std::string_view candidate)
		{
			return std::ranges::any_of(
			           m_players,
			           [&](const auto &entry)
			           {
				           return lower_ascii(entry.second.display_name)
				               == lower_ascii(candidate);
			           })
			    || std::ranges::any_of(
			        m_pending,
			        [&](const auto &entry)
			        {
				        return lower_ascii(entry.second.display_name)
				            == lower_ascii(candidate);
			        });
		};
		if (display_name.empty())
		{
			do
			{
				display_name =
				    "Dummy " + std::to_string(m_next_dummy_index++);
			} while (name_in_use(display_name));
		}
		if (!is_valid_display_name(display_name))
		{
			set_error(
			    "display name must contain 3 to 32 UTF-8 characters");
			return std::nullopt;
		}
		if (name_in_use(display_name))
		{
			set_error("display name is already in use");
			return std::nullopt;
		}

		const player_session *anchor = nullptr;
		for (const auto &[id, player] : m_players)
		{
			if (player.dummy || !player.connection
			    || !player.has_transform)
			{
				continue;
			}
			if (!anchor || id < anchor->id)
			{
				anchor = &player;
			}
		}
		protocol::TransformState transform;
		if (anchor)
		{
			transform = anchor->transform;
		}
		else if (m_store.manifest().spawn_valid)
		{
			transform = m_store.manifest().spawn;
		}
		else
		{
			set_error(
			    "no player transform or configured world spawn is available");
			return std::nullopt;
		}
		const auto dummy_count = std::ranges::count_if(
		    m_players,
		    [](const auto &entry)
		    {
			    return entry.second.dummy;
		    });
		transform.mutable_position()->set_x(
		    transform.position().x()
		    + 2.0F * static_cast<float>(dummy_count + 1));
		transform.mutable_velocity()->Clear();
		transform.set_sequence(1);
		transform.set_client_time_ms(0);

		player_session dummy;
		dummy.id = m_store.allocate_player_id();
		dummy.display_name = std::move(display_name);
		dummy.dummy = true;
		dummy.has_transform = true;
		dummy.transform = std::move(transform);
		dummy.last_sequence = 1;
		dummy.movement_mode = protocol::MOVEMENT_MODE_IDLE;
		dummy.profile = instantiate_starter_profile(
		    m_config.starter_profile,
		    dummy.id,
		    dummy.display_name,
		    m_store.manifest().level_id);
		apply_default_avatar(dummy.profile);
		dummy.profile.set_transform_valid(true);
		*dummy.profile.mutable_last_transform() = dummy.transform;
		dummy.avatar = dummy.profile.avatar();
		dummy.dummy_random_state =
		    dummy.id ^ 0x9e3779b97f4a7c15ULL;

		const auto id = dummy.id;
		auto staged_items = m_items;
		std::string item_error;
		for (const auto &item : dummy.profile.inventory())
		{
			if (!staged_items.register_item(
			        item, item_location::player(id), item_error))
			{
				set_error("could not register dummy inventory: " + item_error);
				return std::nullopt;
			}
		}

		auto [iterator, inserted] =
		    m_players.emplace(id, std::move(dummy));
		(void)inserted;
		m_items = std::move(staged_items);
		protocol::Envelope joined;
		*joined.mutable_player_joined()->mutable_player() =
		    snapshot_of(iterator->second, true);
		broadcast(std::move(joined), reliability::reliable);
		return id;
	}

	void server_core::tick_dummies(time_point now)
	{
		using namespace std::chrono_literals;
		for (auto &[id, player] : m_players)
		{
			(void)id;
			if (!player.dummy)
				continue;
			if (player.dummy_last_update == time_point{})
			{
				player.dummy_last_update = now;
				// Give every client time to finish the asynchronous remote Actor,
				// Soul, inventory, and presentation lifecycle before the first
				// simulated input arrives. Later inputs keep the small player-like
				// jitter below.
				const auto buffer = 2s + 120ms
				    + std::chrono::milliseconds(
				        next_dummy_random(player) % 231ULL);
				player.dummy_next_input_at = now + buffer;
				continue;
			}

			player.dummy_last_update = now;

			if (player.dummy_action_ends_at != time_point{}
			    && now >= player.dummy_action_ends_at)
			{
				if (player.movement_mode != protocol::MOVEMENT_MODE_IDLE)
				{
					player.movement_mode = protocol::MOVEMENT_MODE_IDLE;
					player.transform.mutable_velocity()->Clear();
					player.transform.set_sequence(++player.last_sequence);
					player.transform.set_client_time_ms(milliseconds(now));
				}
				player.dummy_action_ends_at = {};
				const auto buffer = 120ms
				    + std::chrono::milliseconds(
				        next_dummy_random(player) % 231ULL);
				player.dummy_next_input_at = now + buffer;
			}

			if (player.dummy_action_ends_at == time_point{}
			    && now >= player.dummy_next_input_at)
			{
				begin_dummy_input(player, now);
			}
		}
	}

	void server_core::begin_dummy_input(
	    player_session &player,
	    time_point now)
	{
		using namespace std::chrono_literals;
		const auto random = next_dummy_random(player);
		const auto action = random % 100ULL;
		const auto duration = 450ms
		    + std::chrono::milliseconds(
		        next_dummy_random(player) % 451ULL);

		if (action < 75ULL)
		{
			const auto yaw = static_cast<float>(
			    next_dummy_random(player) % 6284ULL)
			    / 1000.0F;
			// CryEngine's actor-forward basis is +Y. Keep the replicated velocity
			// and quaternion aligned so the avatar looks where it tries to walk.
			const auto direction_x = -std::sin(yaw);
			const auto direction_y = std::cos(yaw);
			constexpr float speed = 1.5F;
			player.movement_mode = protocol::MOVEMENT_MODE_WALK;
			auto *velocity = player.transform.mutable_velocity();
			velocity->set_x(direction_x * speed);
			velocity->set_y(direction_y * speed);
			velocity->set_z(0.0F);
			auto *rotation = player.transform.mutable_rotation();
			rotation->set_x(0.0F);
			rotation->set_y(0.0F);
			rotation->set_z(std::sin(yaw * 0.5F));
			rotation->set_w(std::cos(yaw * 0.5F));
			player.transform.set_sequence(++player.last_sequence);
			player.transform.set_client_time_ms(milliseconds(now));
			player.dummy_action_ends_at = now + duration;
			return;
		}

		const auto yaw = static_cast<float>(
		    next_dummy_random(player) % 6284ULL)
		    / 1000.0F;
		player.movement_mode = protocol::MOVEMENT_MODE_IDLE;
		player.transform.mutable_velocity()->Clear();
		auto *rotation = player.transform.mutable_rotation();
		rotation->set_x(0.0F);
		rotation->set_y(0.0F);
		rotation->set_z(std::sin(yaw * 0.5F));
		rotation->set_w(std::cos(yaw * 0.5F));
		player.transform.set_sequence(++player.last_sequence);
		player.transform.set_client_time_ms(milliseconds(now));
		player.dummy_action_ends_at = now + 250ms;
	}

	std::uint64_t server_core::next_dummy_random(player_session &player)
	{
		// SplitMix64 gives each dummy an independent deterministic stream. This
		// keeps server tests reproducible while still making several dummies act
		// differently from one another.
		player.dummy_random_state += 0x9e3779b97f4a7c15ULL;
		auto value = player.dummy_random_state;
		value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
		value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
		return value ^ (value >> 31U);
	}

	bool server_core::remove_dummy(player_id id, time_point now)
	{
		const auto iterator = m_players.find(id);
		if (iterator == m_players.end() || !iterator->second.dummy)
		{
			return false;
		}
		remove_player(id, "dummy removed", close_kind::none, now);
		return true;
	}

	void server_core::server_say(std::string text, time_point now)
	{
		broadcast_system_message(std::move(text), now);
	}

	bool server_core::grant_permission(
	    player_id id,
	    std::string permission,
	    std::string &error)
	{
		const auto found = m_players.find(id);
		if (found == m_players.end() || found->second.dummy
		    || found->second.profile.persistent_id().empty())
		{
			error = "unknown persistent player";
			return false;
		}
		const auto scope = permission;
		const auto accepted = m_permissions.grant(
		    found->second.profile.persistent_id(), std::move(permission), error);
		m_permissions.audit(
		    "console", "permission.grant", found->second.profile.persistent_id(),
		    accepted ? "allowed" : "failed", scope);
		return accepted;
	}

	bool server_core::revoke_permission(
	    player_id id,
	    std::string_view permission,
	    std::string &error)
	{
		const auto found = m_players.find(id);
		if (found == m_players.end() || found->second.dummy
		    || found->second.profile.persistent_id().empty())
		{
			error = "unknown persistent player";
			return false;
		}
		const auto accepted = m_permissions.revoke(
		    found->second.profile.persistent_id(), permission, error);
		m_permissions.audit(
		    "console", "permission.revoke", found->second.profile.persistent_id(),
		    accepted ? "allowed" : "failed", permission);
		return accepted;
	}

	std::vector<std::string> server_core::permissions(player_id id) const
	{
		const auto found = m_players.find(id);
		if (found == m_players.end() || found->second.dummy)
			return {};
		if (found->second.network_full_permissions)
			return {"*"};
		return m_permissions.list(found->second.profile.persistent_id());
	}

	void server_core::broadcast_system_message(
	    std::string text,
	    time_point now,
	    std::optional<connection_id> except)
	{
		if (!is_valid_chat(text))
		{
			return;
		}
		protocol::Envelope envelope;
		auto *chat = envelope.mutable_chat_broadcast();
		chat->set_player_id(0);
		chat->set_display_name("Server");
		chat->set_text(std::move(text));
		chat->set_server_time_ms(milliseconds(now));
		chat->set_channel(protocol::CHAT_CHANNEL_SYSTEM);
		broadcast(std::move(envelope), reliability::reliable, except);
	}

	bool server_core::set_npc_entities_disabled(
	    bool humans_disabled,
	    bool animals_disabled)
	{
		if (m_human_npcs_disabled == humans_disabled
		    && m_animal_npcs_disabled == animals_disabled)
		{
			return false;
		}
		m_human_npcs_disabled = humans_disabled;
		m_animal_npcs_disabled = animals_disabled;
		if (humans_disabled)
			queue_npc_events(m_npcs.disable_kind(protocol::NPC_KIND_HUMAN));
		if (animals_disabled)
			queue_npc_events(m_npcs.disable_kind(protocol::NPC_KIND_ANIMAL));
		protocol::Envelope envelope;
		auto *control = envelope.mutable_server_entity_control();
		control->set_non_player_entities_disabled(
		    humans_disabled && animals_disabled);
		control->set_human_npcs_disabled(humans_disabled);
		control->set_animal_npcs_disabled(animals_disabled);
		broadcast(std::move(envelope), reliability::reliable);
		return true;
	}

	bool server_core::human_npcs_disabled() const
	{
		return m_human_npcs_disabled;
	}

	bool server_core::animal_npcs_disabled() const
	{
		return m_animal_npcs_disabled;
	}

	void server_core::shutdown(std::string reason)
	{
		const auto now = clock::now();
		broadcast_system_message("Server is shutting down.", now);
		for (auto &[id, player] : m_players)
		{
			(void)id;
			if (!player.dummy)
			{
				persist_player(player, now);
			}
			if (!player.connection)
			{
				continue;
			}
			protocol::Envelope envelope;
			envelope.mutable_server_shutdown()->set_reason(reason);
			queue(
			    *player.connection,
			    std::move(envelope),
			    reliability::reliable,
			    close_kind::shutdown);
		}
		m_players.clear();
		m_pending.clear();
		m_initializer.reset();
	}

	std::optional<std::string> server_core::create_profile_claim(
	    player_id id,
	    time_point now)
	{
		if (!m_store.find_by_player_id(id))
		{
			return std::nullopt;
		}
		const auto code = random_hex(16);
		m_claims[id] = {hash_token(code), now + std::chrono::minutes(10)};
		return code;
	}

	const protocol::PropertyCatalog &server_core::property_catalog() const
	{
		return m_properties.catalog();
	}

	const protocol::PropertyLedger &server_core::property_ledger() const
	{
		return m_properties.ledger();
	}

	bool server_core::assign_property_owner(
	    std::string_view property_id,
	    player_id target,
	    std::string &error)
	{
		const auto profile = m_store.find_by_player_id(target);
		if (!profile)
		{
			error = "target player profile does not exist";
			return false;
		}
		if (!m_properties.system_assign_owner(
		        property_id,
		        profile->profile.persistent_id(),
		        random_uuid_v4(),
		        unix_milliseconds(),
		        error))
			return false;
		m_store.save_property_ledger(m_properties.ledger());
		broadcast_home_markers();
		return true;
	}

	bool server_core::grant_property_role(
	    player_id actor,
	    std::string_view property_id,
	    player_id target,
	    protocol::PropertyRole role,
	    std::uint64_t expires_at_ms,
	    std::string &error)
	{
		const auto actor_profile = m_store.find_by_player_id(actor);
		const auto target_profile = m_store.find_by_player_id(target);
		if (!actor_profile || !target_profile)
		{
			error = "actor or target player profile does not exist";
			return false;
		}
		if (!m_properties.grant_role(
		        actor_profile->profile.persistent_id(),
		        property_id,
		        target_profile->profile.persistent_id(),
		        role,
		        random_uuid_v4(),
		        unix_milliseconds(),
		        expires_at_ms,
		        error))
			return false;
		m_store.save_property_ledger(m_properties.ledger());
		broadcast_home_markers();
		return true;
	}

	bool server_core::revoke_property_role(
	    player_id actor,
	    std::string_view assignment_id,
	    std::string &error)
	{
		const auto actor_profile = m_store.find_by_player_id(actor);
		if (!actor_profile)
		{
			error = "actor player profile does not exist";
			return false;
		}
		if (!m_properties.revoke_role(
		        actor_profile->profile.persistent_id(),
		        assignment_id,
		        unix_milliseconds(),
		        error))
			return false;
		m_store.save_property_ledger(m_properties.ledger());
		broadcast_home_markers();
		return true;
	}

	bool server_core::system_revoke_property_role(
	    std::string_view assignment_id,
	    std::string &error)
	{
		if (!m_properties.system_revoke_role(assignment_id, error))
			return false;
		m_store.save_property_ledger(m_properties.ledger());
		broadcast_home_markers();
		return true;
	}

	std::vector<outbound_message> server_core::take_outbound()
	{
		auto result = std::move(m_outbound);
		m_outbound.clear();
		return result;
	}

	std::vector<player_view> server_core::players() const
	{
		std::vector<player_view> result;
		result.reserve(m_players.size());
		for (const auto &[id, player] : m_players)
		{
			result.push_back({
			    id,
			    player.profile.persistent_id(),
			    player.display_name,
			    player.dummy || player.connection.has_value(),
			    player.has_transform,
			    player.last_sequence,
			    player.movement_mode,
			    player.dummy,
			    player.network_role});
		}
		std::ranges::sort(result, {}, &player_view::id);
		return result;
	}

	std::size_t server_core::pending_connection_count() const
	{
		return m_pending.size();
	}

	std::uint64_t server_core::server_tick() const
	{
		return m_server_tick;
	}

	const server_config &server_core::config() const
	{
		return m_config;
	}

	protocol::EnvironmentState server_core::current_environment(
	    time_point now) const
	{
		protocol::EnvironmentState state;
		state.set_revision(m_environment_revision);
		const auto elapsed = m_environment_clock_started
		        && now > m_environment_anchor_time
		    ? now - m_environment_anchor_time
		    : clock::duration::zero();
		const auto world_time_seconds = project_world_time_seconds(
		    m_environment_anchor_world_seconds,
		    m_environment_time_scale,
		    elapsed);
		state.set_world_time_seconds(world_time_seconds);
		state.set_time_of_day_hours(normalize_time_of_day_hours(
		    world_time_seconds / seconds_per_hour));
		state.set_time_scale(m_environment_time_scale);
		state.set_server_time_ms(milliseconds(now));
		state.set_weather_id(m_environment_weather_id);
		state.set_weather_transition_ms(m_environment_weather_transition_ms);
		state.set_weather_revision(m_weather_revision);
		return state;
	}

	bool server_core::set_time_of_day(double hours, time_point now)
	{
		if (!std::isfinite(hours) || hours < 0.0 || hours >= hours_per_day)
			return false;
		advance_environment_clock(now);
		const auto current = current_environment(now).time_of_day_hours();
		if (circular_time_distance_hours(current, hours)
		    < 0.000001)
			return false;
		m_environment_anchor_world_seconds = next_world_time_at_hour(
		    current_environment(now).world_time_seconds(),
		    hours);
		m_environment_anchor_time = now;
		++m_environment_revision;
		broadcast_environment(now);
		return true;
	}

	bool server_core::set_time_scale(float scale, time_point now)
	{
		if (!std::isfinite(scale) || scale < 0.0F
		    || scale > maximum_time_scale)
			return false;
		advance_environment_clock(now);
		if (std::abs(m_environment_time_scale - scale) < 0.000001F)
			return false;
		m_environment_anchor_world_seconds =
		    current_environment(now).world_time_seconds();
		m_environment_anchor_time = now;
		m_environment_time_scale = scale;
		++m_environment_revision;
		broadcast_environment(now);
		return true;
	}

	bool server_core::set_weather(
	    std::uint32_t weather_id,
	    std::uint32_t transition_seconds,
	    time_point now)
	{
		if (weather_id < minimum_weather_id
		    || weather_id > maximum_weather_id
		    || transition_seconds > maximum_weather_transition_ms / 1000U)
			return false;
		advance_environment_clock(now);
		const auto transition_ms = transition_seconds * 1000U;
		if (weather_id == m_environment_weather_id
		    && transition_ms == m_environment_weather_transition_ms)
			return false;
		m_environment_weather_id = weather_id;
		m_environment_weather_transition_ms = transition_ms;
		++m_environment_revision;
		++m_weather_revision;
		broadcast_environment(now);
		return true;
	}

	void server_core::handle_hello(
	    connection_id connection,
	    const protocol::ClientHello &hello,
	    time_point now)
	{
		auto reject_hello = [&](protocol::RejectReason reason, std::string message)
		{
			reject(connection, reason, std::move(message));
		};
		if (hello.version() != kcd2o_version)
		{
			reject_hello(
			    protocol::REJECT_REASON_VERSION_MISMATCH,
			    "KCD2Online version mismatch; server requires "
			        + std::string(kcd2o_version));
			return;
		}
		if (hello.whgame_timestamp() != supported_whgame_timestamp
		    || hello.whgame_image_size() != supported_whgame_image_size)
		{
			reject_hello(
			    protocol::REJECT_REASON_GAME_BUILD_MISMATCH,
			    "unsupported WHGame build");
			return;
		}
		if (!hello.has_runtime()
		    || (hello.runtime().features()
		            & required_client_runtime_capabilities)
		        != required_client_runtime_capabilities
		    || hello.runtime().kcse_version() == 0
		    || hello.runtime().game_version()
		        != supported_kcse_game_version
		    || hello.runtime().release_index()
		        != supported_kcse_release_index)
		{
			reject_hello(
			    protocol::REJECT_REASON_GAME_BUILD_MISMATCH,
			    "KCSE/libKCD2 runtime capabilities are incomplete");
			return;
		}
		if (!is_supported_address_library_identity(hello.runtime()))
		{
			reject_hello(
			    protocol::REJECT_REASON_GAME_BUILD_MISMATCH,
			    "Address Library identity does not match the server allowlist");
			return;
		}
		if (!m_config.account_auth_enabled
		    && hello.password() != m_config.password)
		{
			reject_hello(
			    protocol::REJECT_REASON_AUTHENTICATION_FAILED,
			    "authentication failed");
			return;
		}
		if (!m_config.required_content_hash.empty()
		    && hello.content_hash() != m_config.required_content_hash)
		{
			reject_hello(
			    protocol::REJECT_REASON_CONTENT_MISMATCH,
			    "content hash mismatch");
			return;
		}
		if (!is_valid_display_name(hello.display_name()))
		{
			reject_hello(
			    protocol::REJECT_REASON_MALFORMED_MESSAGE,
			    "display name must contain 3 to 32 UTF-8 characters");
			return;
		}
		const auto normalized_name = lowercase_ascii(hello.display_name());
		if (normalized_name.starts_with("[owner]")
		    || normalized_name.starts_with("[admin]")
		    || normalized_name.starts_with("[moderator]")
		    || normalized_name.starts_with("[support]"))
		{
			reject_hello(
			    protocol::REJECT_REASON_MALFORMED_MESSAGE,
			    "display name uses a reserved network staff marker");
			return;
		}
		auto &pending = m_pending.at(connection);
		pending.display_name = hello.display_name();
		pending.content_hash = hello.content_hash();
		pending.password_accepted = hello.password() == m_config.password;
		pending.stage = pending_stage::authenticate;
		pending.deadline =
		    now + std::chrono::seconds(m_config.handshake_timeout_seconds);
		send_challenge(connection, hello.runtime().features());
	}

	void server_core::handle_authenticate(
	    connection_id connection,
	    const protocol::ClientAuthenticate &message,
	    time_point now)
	{
		auto &pending = m_pending.at(connection);
		const auto reserved_slots = [&]
		{
			return m_players.size()
			    + static_cast<std::size_t>(std::ranges::count_if(
			        m_pending,
			        [&](const auto &entry)
			        {
				        return entry.first != connection
				            && entry.second.persisted.has_value();
			        }));
		};
		if (message.enroll() && !m_config.account_auth_enabled
		    && reserved_slots() >= m_config.max_players)
		{
			reject(
			    connection,
			    protocol::REJECT_REASON_SERVER_FULL,
			    "server is full");
			return;
		}
		std::optional<persisted_profile> profile;
		bool rotate_identity = false;
		bool enrolled_profile = false;
		if (m_config.account_auth_enabled)
		{
			std::string auth_error;
			const auto identity = m_authenticate_account
			    ? m_authenticate_account(message.access_token(), auth_error)
			    : std::nullopt;
			if (!identity)
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_IDENTITY_REQUIRED,
				    auth_error.empty() ? "KCD2Online authentication failed" : auth_error);
				return;
			}
			if (!pending.password_accepted && !identity->join_bypass)
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_AUTHENTICATION_FAILED,
				    "authentication failed");
				return;
			}
			if (m_config.account_whitelist_enabled
			    && !identity->join_bypass && !identity->whitelisted)
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_AUTHENTICATION_FAILED,
				    "account is not on this server's whitelist");
				return;
			}
			pending.network = *identity;
			profile = m_store.find_by_persistent_id(identity->account_id);
			if (!profile)
			{
				if (!identity->join_bypass
				    && reserved_slots() >= m_config.max_players)
				{
					reject(connection, protocol::REJECT_REASON_SERVER_FULL, "server is full");
					return;
				}
				auto created = instantiate_starter_profile(
				    m_config.starter_profile,
				    m_store.allocate_player_id(),
				    pending.display_name,
				    m_store.manifest().level_id);
				apply_default_avatar(created);
				created.set_persistent_id(identity->account_id);
				if (m_store.manifest().spawn_valid)
				{
					created.set_transform_valid(true);
					*created.mutable_last_transform() = m_store.manifest().spawn;
				}
				profile = persisted_profile{
				    hash_token(identity->account_id), std::move(created)};
				enrolled_profile = true;
				m_store.save_profile(profile->identity_hash, profile->profile);
			}
		}
		else if (!message.identity_token().empty())
		{
			profile = m_store.find_by_token(message.identity_token());
			if (!profile)
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_IDENTITY_REQUIRED,
				    "identity token is unknown");
				return;
			}
		}
		else if (!message.claim_code().empty())
		{
			const auto candidate = hash_token(message.claim_code());
			for (auto iterator = m_claims.begin(); iterator != m_claims.end(); ++iterator)
			{
				if (now < iterator->second.expires_at
				    && secure_equal(candidate, iterator->second.code_hash))
				{
					profile = m_store.find_by_player_id(iterator->first);
					m_claims.erase(iterator);
					break;
				}
			}
			if (!profile)
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_IDENTITY_REQUIRED,
				    "claim code is invalid or expired");
				return;
			}
			rotate_identity = true;
		}
		else if (message.enroll())
		{
			const auto duplicate = std::ranges::any_of(
			    m_store.profiles(),
			    [&](const persisted_profile &stored)
			    {
				    return profile_name_matches(
				        stored.profile,
				        pending.display_name);
			    });
			if (duplicate)
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_IDENTITY_REQUIRED,
				    "an identity token is required for this player profile");
				return;
			}
			protocol::PlayerProfile created = instantiate_starter_profile(
			    m_config.starter_profile,
			    m_store.allocate_player_id(),
			    pending.display_name,
			    m_store.manifest().level_id);
			apply_default_avatar(created);
			created.set_persistent_id(random_uuid_v4());
			if (m_store.manifest().spawn_valid)
			{
				created.set_transform_valid(true);
				*created.mutable_last_transform() = m_store.manifest().spawn;
			}
			pending.issued_identity_token = m_generate_token();
			profile = persisted_profile{
			    hash_token(pending.issued_identity_token),
			    std::move(created)};
			enrolled_profile = true;
			m_store.save_profile(profile->identity_hash, profile->profile);
		}
		if (!profile)
		{
			reject(
			    connection,
			    protocol::REJECT_REASON_IDENTITY_REQUIRED,
			    "identity credentials are required");
			return;
		}
		// Enrollment is persisted before the initializer has applied its native
		// profile. If that first bootstrap fails, rebuild the untouched revision-1
		// profile from the current starter template on retry. This also migrates
		// profiles created with an older, non-native starter-item definition.
		if (!enrolled_profile && !m_store.manifest().spawn_valid
		    && profile->profile.revision() == 1
		    && !profile->profile.transform_valid())
		{
			auto refreshed = instantiate_starter_profile(
			    m_config.starter_profile,
			    profile->profile.player_id(),
			    profile->profile.display_name(),
			    m_store.manifest().level_id);
			apply_default_avatar(refreshed);
			refreshed.set_persistent_id(profile->profile.persistent_id());
			profile->profile = std::move(refreshed);
			m_store.save_profile(profile->identity_hash, profile->profile);
		}
		if (!profile->profile.has_avatar()
		    || !is_valid_avatar_descriptor(profile->profile.avatar()))
		{
			apply_default_avatar(profile->profile);
			m_store.save_profile(profile->identity_hash, profile->profile);
		}
		else if (!avatar_allowed(profile->profile.avatar()))
		{
			profile->profile.mutable_avatar()->set_archetype_id(
			    m_config.default_avatar_archetype);
			m_store.save_profile(profile->identity_hash, profile->profile);
		}
		{
			auto staged_items = m_items;
			const auto location =
			    item_location::player(profile->profile.player_id());
			staged_items.erase_location(location);
			std::string item_error;
			for (const auto &item : profile->profile.inventory())
			{
				if (!staged_items.register_item(item, location, item_error))
				{
					throw std::runtime_error(
					    "profile item ownership conflict: " + item_error);
				}
			}
			m_items = std::move(staged_items);
		}
		const auto id = profile->profile.player_id();
		if (!m_players.contains(id)
		    && !(pending.network && pending.network->join_bypass)
		    && reserved_slots() >= m_config.max_players)
		{
			reject(
			    connection,
			    protocol::REJECT_REASON_SERVER_FULL,
			    "server is full");
			return;
		}
		const auto active = m_players.contains(id)
		    && m_players.at(id).connection.has_value();
		const auto authenticating = std::ranges::any_of(
		    m_pending,
		    [&](const auto &entry)
		    {
			    return entry.first != connection && entry.second.persisted
			        && entry.second.persisted->profile.player_id() == id;
		    });
		if (active || authenticating)
		{
			reject(
			    connection,
			    protocol::REJECT_REASON_IDENTITY_IN_USE,
			    "player identity is already connected");
			return;
		}
		const bool display_name_changed =
		    profile->profile.display_name() != pending.display_name;
		if (display_name_changed)
		{
			const auto requested_name = lowercase_ascii(pending.display_name);
			const bool name_in_use = std::ranges::any_of(
			    m_store.profiles(),
			    [&](const persisted_profile &stored)
			    {
				    return stored.profile.player_id() != id
				        && lowercase_ascii(stored.profile.display_name())
				            == requested_name;
			    })
			    || std::ranges::any_of(
			        m_players,
			        [&](const auto &entry)
			        {
				        return entry.first != id
				            && lowercase_ascii(entry.second.display_name)
				                == requested_name;
			        })
			    || std::ranges::any_of(
			        m_pending,
			        [&](const auto &entry)
			        {
				        return entry.first != connection
				            && entry.second.persisted
				            && entry.second.persisted->profile.player_id() != id
				            && lowercase_ascii(entry.second.display_name)
				                == requested_name;
			        });
			if (name_in_use)
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_IDENTITY_REQUIRED,
				    "display name is already in use");
				return;
			}
			profile->profile.set_display_name(pending.display_name);
			profile->profile.set_revision(
			    profile->profile.revision() + 1);
		}
		if (rotate_identity)
		{
			pending.issued_identity_token = m_generate_token();
			profile->identity_hash = hash_token(pending.issued_identity_token);
		}
		if (display_name_changed || rotate_identity)
			m_store.save_profile(profile->identity_hash, profile->profile);
		pending.persisted = std::move(profile);
		pending.resume_token = message.resume_token();
		// A native KCD2 level load can legitimately take much longer than the
		// configured bootstrap estimate. Transport disconnects still remove the
		// pending session, so an authenticated client may wait for the engine's
		// actual level-complete signal without a wall-clock deadline.
		pending.deadline = time_point::max();
		if (m_store.manifest().spawn_valid)
		{
			pending.stage = pending_stage::loading_world;
			send_bootstrap(connection, protocol::BOOTSTRAP_MODE_LOAD);
		}
		else if (!m_initializer)
		{
			m_initializer = connection;
			pending.initializer = true;
			pending.stage = pending_stage::loading_world;
			send_bootstrap(connection, protocol::BOOTSTRAP_MODE_INITIALIZE);
		}
		else
		{
			pending.stage = pending_stage::waiting_for_initializer;
			send_bootstrap(connection, protocol::BOOTSTRAP_MODE_WAIT);
		}
	}

	void server_core::handle_world_ready(
	    connection_id connection,
	    const protocol::ClientWorldReady &message,
	    time_point now)
	{
		auto &pending = m_pending.at(connection);
		const auto manifest_revision = m_store.manifest().revision;
		if (!pending.persisted
		    || message.session_id() != m_store.manifest().session_id
		    || message.manifest_revision() != manifest_revision
		    || message.level_id() != m_store.manifest().level_id)
		{
			reject(
			    connection,
			    protocol::REJECT_REASON_SESSION_MISMATCH,
			    "client loaded a stale or different sandbox session");
			return;
		}
		auto candidate_profile = pending.persisted->profile;
		if (message.has_avatar())
			*candidate_profile.mutable_avatar() = message.avatar();
		if (!message.has_avatar()
		    || !is_valid_avatar_descriptor(message.avatar())
		    || !is_valid_profile(candidate_profile)
		    || !avatar_allowed(message.avatar()))
		{
			reject(
			    connection,
			    protocol::REJECT_REASON_CONTENT_MISMATCH,
			    "client supplied an invalid or disallowed avatar");
			return;
		}
		if (pending.initializer)
		{
			if (!message.initialized_session() || !message.has_initial_spawn())
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_BOOTSTRAP_FAILED,
				    "initializer did not provide a valid spawn");
				return;
			}
			auto spawn = message.initial_spawn();
			if (!is_finite_transform(spawn)
			    || !normalize_rotation(spawn.mutable_rotation()))
			{
				reject(
				    connection,
				    protocol::REJECT_REASON_BOOTSTRAP_FAILED,
				    "initializer spawn is invalid");
				return;
			}
			m_store.set_spawn(spawn);
			pending.persisted->profile.set_transform_valid(true);
			*pending.persisted->profile.mutable_last_transform() = spawn;
			m_store.save_profile(
			    pending.persisted->identity_hash,
			    pending.persisted->profile);
			m_initializer.reset();
		}

		auto persisted = *pending.persisted;
		auto authenticated_network = pending.network;
		m_pending.erase(connection);

		player_session session;
		session.id = persisted.profile.player_id();
		session.display_name = persisted.profile.display_name();
		session.resume_token = m_generate_token();
		session.identity_hash = persisted.identity_hash;
		session.connection = connection;
		if (authenticated_network)
		{
			session.network_role = authenticated_network->network_role;
			session.network_full_permissions =
			    authenticated_network->full_permissions;
			session.network_chat_muted = authenticated_network->chat_muted;
			session.network_voice_muted = authenticated_network->voice_muted;
		}
		session.profile = std::move(persisted.profile);
		session.avatar = message.avatar();
		if (!avatar_allowed(session.avatar))
			session.avatar.set_archetype_id(
			    m_config.default_avatar_archetype);
		session.avatar.set_revision(
		    std::max<std::uint64_t>(
		        1,
		        session.profile.avatar().revision()));
		*session.profile.mutable_avatar() = session.avatar;
		session.last_message_at = now;
		session.last_transform_at = now;
		session.last_persisted_at = now;
		if (session.profile.transform_valid())
		{
			session.has_transform = true;
			session.transform = session.profile.last_transform();
			// Sequences are scoped to a transport/client process. Persist the
			// pose, but start both the accepted and published stream fresh after
			// authentication. Otherwise PlayerJoined advertises the persisted
			// high sequence and observers reject the new client's low sequences.
			session.transform.set_sequence(0);
			session.transform.set_client_time_ms(0);
			session.last_sequence = 0;
		}
		const auto reconnecting = m_players.contains(session.id);
		auto [iterator, inserted] =
		    m_players.insert_or_assign(session.id, std::move(session));
		(void)inserted;
		send_accepted(iterator->second);
		send_world_objects(connection);
		send_world_items(connection);
		protocol::Envelope joined;
		*joined.mutable_player_joined()->mutable_player() =
		    snapshot_of(iterator->second, true);
		broadcast(std::move(joined), reliability::reliable, connection);
		broadcast_system_message(
		    iterator->second.display_name
		        + (reconnecting ? " reconnected." : " joined the server."),
		    now,
		    connection);
		wake_bootstrap_waiters();
	}

	void server_core::handle_world_failed(
	    connection_id connection,
	    const protocol::ClientWorldFailed &message,
	    time_point now)
	{
		(void)now;
		reject(
		    connection,
		    protocol::REJECT_REASON_BOOTSTRAP_FAILED,
		    "client sandbox bootstrap failed: " + message.reason());
	}

	void server_core::handle_profile_update(
	    player_session &player,
	    const protocol::ClientProfileUpdate &message,
	    time_point now)
	{
		const auto reject_profile = [&](std::string_view reason)
		{
			protocol::Envelope rejected;
			auto *response = rejected.mutable_profile_rejected();
			response->set_authoritative_revision(player.profile.revision());
			response->set_reason(reason);
			*response->mutable_authoritative_profile() = player.profile;
			queue(
			    *player.connection,
			    std::move(rejected),
			    reliability::reliable);
		};
		if (!message.has_profile() || !is_valid_profile(message.profile())
		    || message.base_revision() != player.profile.revision()
		    || message.profile().player_id() != player.id
		    || message.profile().persistent_id()
		        != player.profile.persistent_id())
		{
			reject_profile("profile revision or schema conflict");
			return;
		}

		auto accepted = message.profile();
		auto staged_items = m_items;
		const auto player_location = item_location::player(player.id);
		std::unordered_map<std::string, const protocol::InventoryItem *>
		    current_items;
		for (const auto &item : player.profile.inventory())
			current_items.emplace(item.instance_id(), &item);
		std::unordered_map<std::string, const protocol::InventoryItem *>
		    requested_items;
		for (const auto &item : accepted.inventory())
			requested_items.emplace(item.instance_id(), &item);

		std::string item_error;
		std::unordered_set<std::string> merged_away;
		std::unordered_set<std::string> split_created;
		for (const auto &target : accepted.inventory())
		{
			const auto current_target = current_items.find(target.instance_id());
			if (current_target == current_items.end()
			    || target.count() <= current_target->second->count())
			{
				continue;
			}
			auto remaining = target.count() - current_target->second->count();
			for (const auto &source : player.profile.inventory())
			{
				if (remaining == 0 || source.instance_id() == target.instance_id()
				    || !item_ledger::same_stack(source, target, false))
				{
					continue;
				}
				const auto requested_source =
				    requested_items.find(source.instance_id());
				const auto requested_count = requested_source == requested_items.end()
				    ? 0U
				    : requested_source->second->count();
				if (requested_count > source.count())
					continue;
				const auto available = source.count() - requested_count;
				const auto moved = std::min(remaining, available);
				if (moved == 0)
					continue;
				if (!staged_items.merge(
				        source.instance_id(),
				        player_location,
				        target.instance_id(),
				        player_location,
				        moved,
				        item_error))
				{
					reject_profile(item_error);
					return;
				}
				remaining -= moved;
				if (moved == source.count())
					merged_away.insert(source.instance_id());
			}
			// KCD2 can grow a stack through native systems that have no network
			// transaction of their own (shops, quest rewards, crafting and
			// authored world loot). Known merge sources are consumed above; any
			// remainder is accepted as game-origin stack growth below.
		}

		for (const auto &created : accepted.inventory())
		{
			if (current_items.contains(created.instance_id())
			    || staged_items.find(created.instance_id()))
			{
				continue;
			}
			const protocol::InventoryItem *split_source = nullptr;
			for (const auto &source : player.profile.inventory())
			{
				const auto requested_source =
				    requested_items.find(source.instance_id());
				if (requested_source == requested_items.end()
				    || requested_source->second->count() >= source.count()
				    || source.count() - requested_source->second->count()
				        != created.count()
				    || !item_ledger::same_stack(source, created, false))
				{
					continue;
				}
				if (split_source)
				{
					split_source = nullptr;
					break;
				}
				split_source = &source;
			}
			if (!split_source)
				continue;
			if (!staged_items.split(
			        split_source->instance_id(),
			        player_location,
			        created.instance_id(),
			        created.count(),
			        player_location,
			        item_error))
			{
				reject_profile(item_error);
				return;
			}
			split_created.insert(created.instance_id());
		}

		for (const auto &item : accepted.inventory())
		{
			const auto current = current_items.find(item.instance_id());
			if (current != current_items.end())
			{
				if (!staged_items.replace_item(
				        item, player_location, item_error))
				{
					reject_profile(item_error);
					return;
				}
				continue;
			}

			const auto *source = staged_items.find(item.instance_id());
			if (!source)
			{
				// The native game is authoritative for acquisitions which do not
				// expose a multiplayer transaction (trading, rewards and initial
				// authored pickups). Register the first observation atomically. A
				// UUID already owned elsewhere still takes the conflict path below.
				if (!staged_items.register_item(
				        item, player_location, item_error))
				{
					reject_profile(item_error);
					return;
				}
				continue;
			}
			if (source->location.kind == item_location_kind::player
			    && !split_created.contains(item.instance_id()))
			{
				reject_profile("item is already owned by another player");
				return;
			}
			if (!item_ledger::same_stack(source->item, item))
			{
				reject_profile(
				    "item definition or stack values differ from its source");
				return;
			}
			if ((source->location != player_location
			        && !staged_items.move(
			            item.instance_id(),
			            source->location,
			            player_location,
			            item_error))
			    || !staged_items.replace_item(item, player_location, item_error))
			{
				reject_profile(item_error);
				return;
			}
		}
		for (const auto &item : player.profile.inventory())
		{
			const auto still_present = std::ranges::find_if(
			    accepted.inventory(),
			    [&](const protocol::InventoryItem &candidate)
			    { return candidate.instance_id() == item.instance_id(); });
			if (still_present == accepted.inventory().end()
			    && !merged_away.contains(item.instance_id())
			    && !staged_items.erase(
			        item.instance_id(), player_location, item_error))
			{
				reject_profile(item_error);
				return;
			}
		}

		accepted.set_player_id(player.id);
		accepted.set_persistent_id(player.profile.persistent_id());
		accepted.set_display_name(player.display_name);
		accepted.set_level_id(m_store.manifest().level_id);
		accepted.set_revision(player.profile.revision() + 1);
		if (accepted.has_avatar()
		    && !avatar_allowed(accepted.avatar()))
		{
			accepted.mutable_avatar()->set_archetype_id(
			    m_config.default_avatar_archetype);
		}
		accepted.set_transform_valid(player.has_transform);
		if (player.has_transform)
		{
			*accepted.mutable_last_transform() = player.transform;
		}
		player.profile = std::move(accepted);
		m_items = std::move(staged_items);
		persist_player(player, now);
		remove_owned_items_from_world();
		protocol::Envelope response;
		response.mutable_profile_accepted()->set_revision(
		    player.profile.revision());
		queue(*player.connection, std::move(response), reliability::reliable);
	}

	void server_core::handle_world_object_update(
	    player_session &player,
	    const protocol::ClientWorldObjectUpdate &message)
	{
		const auto reject_state = [&](
		    const protocol::WorldObjectState &state,
		    std::string_view reason = "world object revision conflict")
		{
			protocol::Envelope rejected;
			auto *response = rejected.mutable_world_object_rejected();
			*response->mutable_authoritative_state() = state;
			response->set_reason(reason);
			queue(
			    *player.connection,
			    std::move(rejected),
			    reliability::reliable);
		};
		if (!message.has_state()
		    || !is_valid_world_object_state(message.state(), false))
		{
			reject(
			    *player.connection,
			    protocol::REJECT_REASON_MALFORMED_MESSAGE,
			    "world object update is invalid");
			return;
		}

		const auto guid = message.state().entity_guid();
		const auto requested = message.state().kind()
		        == protocol::WORLD_OBJECT_KIND_CONTAINER
		    ? property::capability::use_container
		    : property::capability::enter;
		if (!m_properties.authorize(
		        player.profile.persistent_id(),
		        guid,
		        requested,
		        unix_milliseconds()))
		{
			protocol::WorldObjectState authoritative = message.state();
			if (const auto current = m_world_objects.find(guid);
			    current != m_world_objects.end())
				authoritative = current->second;
			else
			{
				authoritative.set_revision(1);
				authoritative.set_opened(false);
				authoritative.set_has_inventory(false);
				authoritative.clear_inventory();
			}
			reject_state(authoritative, "property access denied");
			return;
		}
		auto found = m_world_objects.find(guid);
		if (found != m_world_objects.end()
		    && (message.base_revision() != found->second.revision()
		        || message.state().kind() != found->second.kind()))
		{
			reject_state(found->second);
			return;
		}
		if (found == m_world_objects.end() && message.base_revision() != 0)
		{
			protocol::WorldObjectState empty = message.state();
			empty.set_revision(1);
			empty.set_opened(false);
			empty.set_has_inventory(false);
			empty.clear_inventory();
			reject_state(empty);
			return;
		}

		auto accepted = message.state();
		auto staged_items = m_items;
		auto updated_profile = player.profile;
		const auto container_location = item_location::container(guid);
		const auto player_location = item_location::player(player.id);
		bool profile_changed = false;
		std::string item_error;

		std::unordered_map<std::string, const protocol::InventoryItem *>
		    previous_items;
		if (found != m_world_objects.end())
		{
			for (const auto &item : found->second.inventory())
				previous_items.emplace(item.instance_id(), &item);
		}

		const auto remove_from_profile = [&](std::string_view instance)
		{
			auto *inventory = updated_profile.mutable_inventory();
			for (auto index = inventory->size(); index-- > 0;)
			{
				if (inventory->Get(index).instance_id() == instance)
					inventory->DeleteSubrange(index, 1);
			}
			auto *quick = updated_profile.mutable_quick_access_slots();
			for (auto index = quick->size(); index-- > 0;)
			{
				if (quick->Get(index).instance_id() == instance)
					quick->DeleteSubrange(index, 1);
			}
		};
		const auto unique_profile_stack = [&] (
		    const protocol::InventoryItem &stack,
		    std::string_view excluded,
		    std::uint32_t minimum_count)
		    -> protocol::InventoryItem *
		{
			protocol::InventoryItem *match = nullptr;
			for (auto &candidate : *updated_profile.mutable_inventory())
			{
				if (candidate.instance_id() == excluded
				    || candidate.count() < minimum_count
				    || !item_ledger::same_stack(candidate, stack, false))
				{
					continue;
				}
				if (match)
					return nullptr;
				match = &candidate;
			}
			return match;
		};

		for (const auto &item : accepted.inventory())
		{
			if (const auto previous = previous_items.find(item.instance_id());
			    previous != previous_items.end())
			{
				if (!item_ledger::same_stack(*previous->second, item, false)
				    || item.count() < previous->second->count())
				{
					reject_state(
					    found->second,
					    "container item values cannot change without a transfer");
					return;
				}
				if (item.count() > previous->second->count())
				{
					const auto added = item.count() - previous->second->count();
					auto *source = unique_profile_stack(
					    item, item.instance_id(), added);
					if (!source
					    || !staged_items.merge(
					        source->instance_id(),
					        player_location,
					        item.instance_id(),
					        container_location,
					        added,
					        item_error))
					{
						reject_state(
						    found->second,
						    item_error.empty()
						        ? "container stack growth has no unique player source"
						        : item_error);
						return;
					}
					const auto source_id = source->instance_id();
					if (source->count() == added)
						remove_from_profile(source_id);
					else
						source->set_count(source->count() - added);
					profile_changed = true;
				}
				continue;
			}

			const auto *source = staged_items.find(item.instance_id());
			if (!source)
			{
				if (found != m_world_objects.end())
				{
					auto *split_source = unique_profile_stack(
					    item, item.instance_id(), item.count() + 1);
					if (!split_source
					    || !staged_items.split(
					        split_source->instance_id(),
					        player_location,
					        item.instance_id(),
					        item.count(),
					        container_location,
					        item_error))
					{
						reject_state(
						    found->second,
						    item_error.empty()
						        ? "new container stack has no unique split source"
						        : item_error);
						return;
					}
					split_source->set_count(
					    split_source->count() - item.count());
					profile_changed = true;
					continue;
				}
				if (!staged_items.register_item(
				        item, container_location, item_error))
				{
					reject_state(
					    accepted, item_error);
					return;
				}
				continue;
			}
			if (source->location != player_location
			    || !item_ledger::same_stack(source->item, item))
			{
				reject_state(
				    found == m_world_objects.end() ? accepted : found->second,
				    "container deposit does not match a player-owned item");
				return;
			}
			if (!staged_items.move(
			        item.instance_id(),
			        player_location,
			        container_location,
			        item_error)
			    || !staged_items.replace_item(
			        item, container_location, item_error))
			{
				reject_state(
				    found == m_world_objects.end() ? accepted : found->second,
				    item_error);
				return;
			}
			remove_from_profile(item.instance_id());
			profile_changed = true;
		}

		if (found != m_world_objects.end())
		{
			for (const auto &item : found->second.inventory())
			{
				const auto remains = std::ranges::find_if(
				    accepted.inventory(),
				    [&](const protocol::InventoryItem &candidate)
				    { return candidate.instance_id() == item.instance_id(); });
				if (remains != accepted.inventory().end())
					continue;
				const auto *container_entry =
				    staged_items.find(item.instance_id());
				const auto count = item.count();
				auto *merge_target = unique_profile_stack(
				    item, item.instance_id(), 1);
				if (merge_target)
				{
					if (!staged_items.merge(
					        item.instance_id(),
					        container_location,
					        merge_target->instance_id(),
					        player_location,
					        count,
					        item_error))
					{
						reject_state(found->second, item_error);
						return;
					}
					merge_target->set_count(merge_target->count() + count);
				}
				else if (!container_entry
				    || !staged_items.move(
				        item.instance_id(),
				        container_location,
				        player_location,
				        item_error))
				{
					reject_state(found->second, item_error);
					return;
				}
				else
					*updated_profile.add_inventory() = item;
				profile_changed = true;
			}
		}

		accepted.set_revision(
		    found == m_world_objects.end() ? 1 : found->second.revision() + 1);
		if (profile_changed)
		{
			updated_profile.set_revision(player.profile.revision() + 1);
			player.profile = std::move(updated_profile);
			rebuild_avatar_equipment(player);
			persist_player(player, m_current_time);
		}
		m_items = std::move(staged_items);
		m_world_objects.insert_or_assign(guid, accepted);
		persist_world_objects();

		protocol::Envelope response;
		auto *ack = response.mutable_world_object_accepted();
		ack->set_entity_guid(guid);
		ack->set_revision(accepted.revision());
		if (profile_changed)
			*ack->mutable_authoritative_profile() = player.profile;
		queue(*player.connection, std::move(response), reliability::reliable);

		protocol::Envelope updated;
		*updated.mutable_world_object_updated()->mutable_state() = accepted;
		broadcast(
		    std::move(updated),
		    reliability::reliable,
		    player.connection);
	}

	void server_core::handle_world_item_update(
	    player_session &player,
	    const protocol::ClientWorldItemUpdate &message)
	{
		const auto reject_state = [&] (
		    const protocol::WorldItemState &state,
		    std::string_view reason = "world item revision conflict")
		{
			protocol::Envelope rejected;
			auto *response = rejected.mutable_world_item_rejected();
			*response->mutable_authoritative_state() = state;
			response->set_reason(reason);
			queue(
			    *player.connection,
			    std::move(rejected),
			    reliability::reliable);
		};
		if (!message.has_state()
		    || !is_valid_world_item_state(message.state(), false))
		{
			reject(
			    *player.connection,
			    protocol::REJECT_REASON_MALFORMED_MESSAGE,
			    "world item update is invalid");
			return;
		}

		const auto &instance = message.state().instance_id();
		auto found = m_world_items.find(instance);
		if (found != m_world_items.end()
		    && message.base_revision() != found->second.revision())
		{
			reject_state(found->second);
			return;
		}
		if (found == m_world_items.end() && message.base_revision() != 0)
		{
			auto absent = message.state();
			absent.set_revision(1);
			absent.set_present(false);
			reject_state(absent);
			return;
		}
		if (found == m_world_items.end()
		    && m_world_items.size() >= max_world_items)
		{
			auto absent = message.state();
			absent.set_revision(1);
			absent.set_present(false);
			reject_state(absent);
			return;
		}

		auto accepted = message.state();
		(void)normalize_rotation(accepted.mutable_transform()->mutable_rotation());
		auto staged_items = m_items;
		auto updated_profile = player.profile;
		const auto player_location = item_location::player(player.id);
		const auto world_location = item_location::world();
		bool profile_changed = false;
		std::string item_error;

		const auto reject_requested = [&](std::string_view reason)
		{
			if (found != m_world_items.end())
				reject_state(found->second, reason);
			else
			{
				auto absent = accepted;
				absent.set_revision(1);
				absent.set_present(false);
				reject_state(absent, reason);
			}
		};
		const auto remove_from_profile = [&](std::string_view item_instance)
		{
			auto *inventory = updated_profile.mutable_inventory();
			for (auto index = inventory->size(); index-- > 0;)
			{
				if (inventory->Get(index).instance_id() == item_instance)
					inventory->DeleteSubrange(index, 1);
			}
			auto *quick = updated_profile.mutable_quick_access_slots();
			for (auto index = quick->size(); index-- > 0;)
			{
				if (quick->Get(index).instance_id() == item_instance)
					quick->DeleteSubrange(index, 1);
			}
		};

		if (accepted.present())
		{
			if (found != m_world_items.end() && found->second.present())
			{
				const auto *entry = staged_items.find(instance);
				if (!entry || entry->location != world_location
				    || !item_ledger::same_stack(entry->item, accepted.item()))
				{
					reject_requested(
					    "world item identity or stack values changed");
					return;
				}
			}
			else
			{
				const auto source_instance = message.source_instance_id().empty()
				    ? instance
				    : message.source_instance_id();
				const auto *source = staged_items.find(source_instance);
				if (!source || source->location != player_location)
				{
					reject_requested(
					    "world drop has no matching player-owned source");
					return;
				}

				if (source_instance == instance)
				{
					if ((message.transfer_count() != 0
					        && message.transfer_count() != source->item.count())
					    || !item_ledger::same_stack(
					        source->item, accepted.item()))
					{
						reject_requested(
						    "complete drop must preserve item identity and values");
						return;
					}
					if (!staged_items.move(
					        instance,
					        player_location,
					        world_location,
					        item_error)
					    || !staged_items.replace_item(
					        accepted.item(), world_location, item_error))
					{
						reject_requested(item_error);
						return;
					}
					remove_from_profile(instance);
				}
				else
				{
					const auto count = message.transfer_count() == 0
					    ? accepted.item().count()
					    : message.transfer_count();
					if (accepted.item().count() != count
					    || !item_ledger::same_stack(
					        source->item, accepted.item(), false)
					    || !staged_items.split(
					        source_instance,
					        player_location,
					        instance,
					        count,
					        world_location,
					        item_error))
					{
						reject_requested(
						    item_error.empty()
						        ? "split drop values do not match the source stack"
						        : item_error);
						return;
					}
					for (auto &profile_item :
					     *updated_profile.mutable_inventory())
					{
						if (profile_item.instance_id() == source_instance)
							profile_item.set_count(
							    profile_item.count() - count);
					}
				}
				profile_changed = true;
			}
		}
		else if (found != m_world_items.end() && found->second.present())
		{
			const auto *source = staged_items.find(instance);
			if (!source || source->location != world_location
			    || !item_ledger::same_stack(source->item, found->second.item())
			    || !item_ledger::same_stack(source->item, accepted.item()))
			{
				reject_requested("world pickup does not match the stored item");
				return;
			}
			if (!staged_items.move(
			        instance,
			        world_location,
			        player_location,
			        item_error))
			{
				reject_requested(item_error);
				return;
			}
			*updated_profile.add_inventory() = source->item;
			profile_changed = true;
		}
		else if (found == m_world_items.end())
		{
			// The client's initial world scan deliberately does not upload every
			// authored item. Its first disappearance into the player's inventory
			// is therefore the atomic registration + pickup transaction.
			const auto *existing = staged_items.find(instance);
			if (!existing)
			{
				if (!staged_items.register_item(
				        accepted.item(), player_location, item_error))
				{
					reject_requested(item_error);
					return;
				}
				*updated_profile.add_inventory() = accepted.item();
				profile_changed = true;
			}
			else if (existing->location != player_location
			    || !item_ledger::same_stack(existing->item, accepted.item()))
			{
				reject_requested(
				    "authored world pickup conflicts with registered ownership");
				return;
			}
		}

		if (profile_changed)
		{
			updated_profile.set_revision(player.profile.revision() + 1);
			player.profile = std::move(updated_profile);
			rebuild_avatar_equipment(player);
			persist_player(player, m_current_time);
		}
		m_items = std::move(staged_items);

		accepted.set_revision(
		    found == m_world_items.end() ? 1 : found->second.revision() + 1);
		m_world_items.insert_or_assign(instance, accepted);
		persist_world_items();

		protocol::Envelope response;
		auto *ack = response.mutable_world_item_accepted();
		ack->set_instance_id(instance);
		ack->set_revision(accepted.revision());
		if (profile_changed)
			*ack->mutable_authoritative_profile() = player.profile;
		queue(*player.connection, std::move(response), reliability::reliable);

		protocol::Envelope updated;
		*updated.mutable_world_item_updated()->mutable_state() = accepted;
		broadcast(
		    std::move(updated),
		    reliability::reliable,
		    player.connection);
	}

	void server_core::handle_npc_discovery(
	    player_session &player,
	    const protocol::ClientNpcDiscovery &message,
	    time_point now)
	{
		m_npcs.observe(
		    player.id,
		    message,
		    player.has_transform ? &player.transform : nullptr,
		    !m_human_npcs_disabled,
		    !m_animal_npcs_disabled,
		    now);
		const auto positions = player_positions();
		queue_npc_events(m_npcs.reconcile(positions, now));
	}

	void server_core::handle_npc_update(
	    player_session &player,
	    const protocol::ClientNpcUpdate &message,
	    time_point now)
	{
		// Stale/revoked leases are expected during handoff and packet reordering;
		// ignore them instead of disconnecting an otherwise valid client.
		(void)m_npcs.update(player.id, message, now);
	}

	void server_core::handle_transform(
	    player_session &player,
	    const protocol::ClientTransform &message,
	    time_point now)
	{
		if (!message.has_transform())
		{
			reject(
			    *player.connection,
			    protocol::REJECT_REASON_MALFORMED_MESSAGE,
			    "ClientTransform has no transform");
			return;
		}
		auto candidate = message.transform();
		if (!is_finite_transform(candidate)
		    || !normalize_rotation(candidate.mutable_rotation()))
		{
			reject(
			    *player.connection,
			    protocol::REJECT_REASON_MALFORMED_MESSAGE,
			    "transform contains invalid values");
			return;
		}
		if (candidate.sequence() <= player.last_sequence)
		{
			return;
		}
		if (player.frozen)
		{
			protocol::Envelope correction;
			*correction.mutable_state_correction()
			     ->mutable_accepted_transform() = player.frozen_transform;
			correction.mutable_state_correction()->set_reason(
			    "player movement is frozen by a game master");
			queue(
			    *player.connection,
			    std::move(correction),
			    reliability::reliable);
			return;
		}
		if (player.has_transform)
		{
			const auto seconds = std::clamp(
			    std::chrono::duration<float>(now - player.last_transform_at).count(),
			    0.001F,
			    1.0F);
			const auto &from = player.transform.position();
			const auto &to = candidate.position();
			const auto distance = std::sqrt(
			    std::pow(to.x() - from.x(), 2.0F)
			    + std::pow(to.y() - from.y(), 2.0F)
			    + std::pow(to.z() - from.z(), 2.0F));
			const auto allowed =
			    m_config.max_player_speed_mps * seconds
			    + m_config.movement_tolerance_m;
			if (distance > allowed)
			{
				protocol::Envelope correction;
				*correction.mutable_state_correction()
				     ->mutable_accepted_transform() = player.transform;
				correction.mutable_state_correction()->set_reason(
				    "movement exceeded the server limit");
				queue(
				    *player.connection,
				    std::move(correction),
				    reliability::reliable);
				return;
			}
		}
		player.transform = std::move(candidate);
		player.last_sequence = player.transform.sequence();
		player.last_transform_at = now;
		player.has_transform = true;
		player.movement_mode = movement_mode_for(player.transform);
	}

	void server_core::handle_avatar_update(
	    player_session &player,
	    const protocol::ClientAvatarUpdate &message,
	    time_point now)
	{
		const auto cutoff = now - std::chrono::seconds(1);
		while (!player.avatar_update_times.empty()
		    && player.avatar_update_times.front() < cutoff)
		{
			player.avatar_update_times.pop_front();
		}
		if (player.avatar_update_times.size() >= 4)
		{
			return;
		}
		player.avatar_update_times.push_back(now);

		auto candidate_profile = player.profile;
		if (message.has_avatar())
			*candidate_profile.mutable_avatar() = message.avatar();
		if (!message.has_avatar()
		    || !is_valid_avatar_descriptor(message.avatar())
		    || !is_valid_profile(candidate_profile))
		{
			protocol::Envelope rejected;
			auto *response = rejected.mutable_avatar_rejected();
			*response->mutable_authoritative_avatar() = player.avatar;
			response->set_reason(
			    "avatar descriptor is invalid or disallowed");
			queue(
			    *player.connection,
			    std::move(rejected),
			    reliability::reliable);
			return;
		}
		if (message.base_revision() != player.avatar.revision())
		{
			protocol::Envelope rejected;
			auto *response = rejected.mutable_avatar_rejected();
			*response->mutable_authoritative_avatar() = player.avatar;
			response->set_reason("avatar revision conflict");
			queue(
			    *player.connection,
			    std::move(rejected),
			    reliability::reliable);
			return;
		}

		player.avatar = message.avatar();
		const bool normalized_archetype = !avatar_allowed(player.avatar);
		if (normalized_archetype)
			player.avatar.set_archetype_id(
			    m_config.default_avatar_archetype);
		player.avatar.set_revision(player.avatar.revision() + 1);
		*player.profile.mutable_avatar() = player.avatar;
		persist_player(player, now);

		protocol::Envelope accepted;
		if (normalized_archetype)
		{
			auto *fallback = accepted.mutable_avatar_rejected();
			*fallback->mutable_authoritative_avatar() = player.avatar;
			fallback->set_reason(
			    "unknown avatar Soul ID was replaced with the server default");
		}
		else
		{
			accepted.mutable_avatar_accepted()->set_revision(
			    player.avatar.revision());
		}
		queue(
		    *player.connection,
		    std::move(accepted),
		    reliability::reliable);

		protocol::Envelope updated;
		auto *broadcast_update = updated.mutable_player_avatar_updated();
		broadcast_update->set_player_id(player.id);
		*broadcast_update->mutable_avatar() = player.avatar;
		broadcast(
		    std::move(updated),
		    reliability::reliable,
		    player.connection);
	}

	void server_core::handle_chat(
	    player_session &player,
	    const protocol::ChatSend &message,
	    time_point now)
	{
		if (player.network_chat_muted)
		{
			send_system_message(player, "Dein Netzwerk-Chat ist stummgeschaltet.", now);
			return;
		}
		if (!is_valid_chat(message.text()))
		{
			return;
		}
		const auto cutoff = now - std::chrono::seconds(10);
		while (!player.chat_times.empty() && player.chat_times.front() < cutoff)
		{
			player.chat_times.pop_front();
		}
		if (player.chat_times.size() >= 5)
		{
			return;
		}
		player.chat_times.push_back(now);

		auto text = std::string_view(message.text());
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
			text.remove_prefix(1);
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
			text.remove_suffix(1);
		if (text.empty())
			return;

		auto split_command = [](std::string_view input)
		{
			const auto separator = input.find_first_of(" \t");
			auto command = input.substr(0, separator);
			auto body = separator == std::string_view::npos
			    ? std::string_view{}
			    : input.substr(separator + 1);
			while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front())))
				body.remove_prefix(1);
			return std::pair{command, body};
		};

		protocol::ChatChannel channel = protocol::CHAT_CHANNEL_SAY;
		float range = m_config.chat_say_range_m;
		if (text.front() == '/')
		{
			const auto [raw_command, body] = split_command(text);
			const auto command = lower_ascii(raw_command);
			if (command == "/w" || command == "/whisper")
			{
				channel = protocol::CHAT_CHANNEL_WHISPER;
				range = m_config.chat_whisper_range_m;
			}
			else if (command == "/s" || command == "/say")
			{
				channel = protocol::CHAT_CHANNEL_SAY;
				range = m_config.chat_say_range_m;
			}
			else if (command == "/y" || command == "/shout")
			{
				channel = protocol::CHAT_CHANNEL_SHOUT;
				range = m_config.chat_shout_range_m;
			}
			else if (command == "/me")
			{
				channel = protocol::CHAT_CHANNEL_EMOTE;
				range = m_config.chat_say_range_m;
			}
			else if (command == "/do")
			{
				channel = protocol::CHAT_CHANNEL_SCENE;
				range = m_config.chat_say_range_m;
			}
			else if (command == "/ooc")
			{
				if (!m_config.chat_ooc_enabled)
				{
					send_system_message(player, "Der OOC-Kanal ist deaktiviert.", now);
					return;
				}
				if (body.empty())
				{
					send_system_message(player, "Verwendung: /ooc <Nachricht>", now);
					return;
				}
				protocol::Envelope envelope;
				auto *chat = envelope.mutable_chat_broadcast();
				chat->set_player_id(player.id);
				chat->set_display_name(player.display_name);
				chat->set_text(body);
				chat->set_server_time_ms(milliseconds(now));
				chat->set_channel(protocol::CHAT_CHANNEL_OOC);
				chat->set_network_role(network_role(player.network_role));
				broadcast(std::move(envelope), reliability::reliable);
				return;
			}
			else
			{
				(void)handle_admin_chat(player, text, now);
				return;
			}
			if (body.empty())
			{
				send_system_message(player, "Nach dem Chatbefehl fehlt eine Nachricht.", now);
				return;
			}
			text = body;
		}

		send_spatial_chat(player, std::string(text), channel, range, now);
	}

	void server_core::handle_voice(
	    player_session &player,
	    const protocol::ClientVoiceFrame &message,
	    time_point now)
	{
		if (player.network_voice_muted || !m_config.voice_enabled || !player.connection
		    || !player.has_transform)
			return;

		const auto cutoff = now - std::chrono::seconds(1);
		while (!player.voice_frame_times.empty()
		    && player.voice_frame_times.front() < cutoff)
			player.voice_frame_times.pop_front();
		if (player.voice_frame_times.size()
		    >= m_config.voice_max_frames_per_second)
			return;
		player.voice_frame_times.push_back(now);

		float range = m_config.voice_normal_range_m;
		switch (message.range())
		{
		case protocol::VOICE_RANGE_WHISPER:
			range = m_config.voice_whisper_range_m;
			break;
		case protocol::VOICE_RANGE_SHOUT:
			range = m_config.voice_shout_range_m;
			break;
		default:
			break;
		}

		protocol::Envelope envelope;
		auto *voice = envelope.mutable_server_voice_frame();
		voice->set_player_id(player.id);
		voice->set_sequence(message.sequence());
		voice->set_capture_time_ms(message.capture_time_ms());
		voice->set_range(message.range());
		voice->set_opus(message.opus());
		voice->set_visemes(message.visemes());
		voice->set_end_of_talkspurt(message.end_of_talkspurt());

		const auto &origin = player.transform.position();
		const auto range_squared = range * range;
		for (const auto &[id, recipient] : m_players)
		{
			if (id == player.id || !recipient.connection
			    || !recipient.has_transform)
				continue;
			const auto &position = recipient.transform.position();
			const auto dx = position.x() - origin.x();
			const auto dy = position.y() - origin.y();
			const auto dz = position.z() - origin.z();
			if (dx * dx + dy * dy + dz * dz <= range_squared)
				queue(
				    *recipient.connection,
				    envelope,
				    reliability::unreliable);
		}
	}

	void server_core::send_chat_message(
	    connection_id connection,
	    player_id sender,
	    std::string_view display_name,
	    std::string text,
	    protocol::ChatChannel channel,
	    time_point now)
	{
		if (!is_valid_chat(text))
			return;
		protocol::Envelope envelope;
		auto *chat = envelope.mutable_chat_broadcast();
		chat->set_player_id(sender);
		chat->set_display_name(display_name);
		chat->set_text(std::move(text));
		chat->set_server_time_ms(milliseconds(now));
		chat->set_channel(channel);
		if (const auto found = m_players.find(sender);
		    found != m_players.end())
			chat->set_network_role(network_role(found->second.network_role));
		queue(connection, std::move(envelope), reliability::reliable);
	}

	void server_core::send_system_message(
	    player_session &player,
	    std::string text,
	    time_point now,
	    protocol::ChatChannel channel)
	{
		if (player.connection)
			send_chat_message(*player.connection, 0, "Server", std::move(text), channel, now);
	}

	void server_core::send_spatial_chat(
	    const player_session &sender,
	    std::string text,
	    protocol::ChatChannel channel,
	    float range_m,
	    time_point now)
	{
		if (!sender.has_transform)
		{
			if (sender.connection)
				send_chat_message(
				    *sender.connection, 0, "Server",
				    "Lokaler Chat ist erst nach dem Weltbeitritt verfuegbar.",
				    protocol::CHAT_CHANNEL_SYSTEM, now);
			return;
		}
		const auto &origin = sender.transform.position();
		const auto range_squared = range_m * range_m;
		for (const auto &[id, recipient] : m_players)
		{
			if (!recipient.connection || !recipient.has_transform)
				continue;
			const auto &position = recipient.transform.position();
			const auto dx = position.x() - origin.x();
			const auto dy = position.y() - origin.y();
			const auto dz = position.z() - origin.z();
			if (dx * dx + dy * dy + dz * dz <= range_squared)
			{
				send_chat_message(
				    *recipient.connection, sender.id, sender.display_name,
				    text, channel, now);
			}
		}
	}

	bool server_core::teleport_player(
	    player_session &target,
	    const protocol::TransformState &destination,
	    std::string reason,
	    time_point now)
	{
		if (!target.connection || !target.has_transform
		    || !is_finite_transform(destination))
			return false;
		target.transform = destination;
		target.transform.set_sequence(++target.last_sequence);
		target.transform.set_client_time_ms(milliseconds(now));
		target.transform.mutable_velocity()->set_x(0.0F);
		target.transform.mutable_velocity()->set_y(0.0F);
		target.transform.mutable_velocity()->set_z(0.0F);
		target.last_transform_at = now;
		target.movement_mode = protocol::MOVEMENT_MODE_IDLE;
		if (target.frozen)
			target.frozen_transform = target.transform;
		protocol::Envelope correction;
		*correction.mutable_state_correction()->mutable_accepted_transform() =
		    target.transform;
		correction.mutable_state_correction()->set_reason(std::move(reason));
		queue(*target.connection, std::move(correction), reliability::reliable);
		return true;
	}

	bool server_core::handle_admin_chat(
	    player_session &player,
	    std::string_view text,
	    time_point now)
	{
		std::istringstream input{std::string(text)};
		std::string command;
		input >> command;
		command = lower_ascii(command);
		const auto actor = player.profile.persistent_id();
		auto require = [&](std::string_view scope, std::string_view action, std::string_view target = {})
		{
			if (player.network_full_permissions
			    || m_permissions.has(actor, scope))
				return true;
			m_permissions.audit(actor, action, target, "denied", scope);
			send_system_message(player, "Dafuer fehlt die Berechtigung: " + std::string(scope), now, protocol::CHAT_CHANNEL_ADMIN);
			return false;
		};
		auto parse_target = [&](player_id id) -> player_session *
		{
			const auto found = m_players.find(id);
			return found == m_players.end() ? nullptr : &found->second;
		};

		if (command == "/adminhelp")
		{
			send_system_message(
			    player,
			    "GM: /players, /announce, /kick, /goto, /bring, /freeze, /unfreeze, /perm",
			    now, protocol::CHAT_CHANNEL_ADMIN);
			return true;
		}
		if (command == "/players")
		{
			if (!require("admin.players", "players.list"))
				return true;
			for (const auto &entry : players())
				send_system_message(player, std::to_string(entry.id) + " - " + entry.display_name + (entry.connected ? " [online]" : " [reconnecting]"), now, protocol::CHAT_CHANNEL_ADMIN);
			m_permissions.audit(actor, "players.list", "", "allowed");
			return true;
		}
		if (command == "/announce")
		{
			if (!require("admin.announce", "announce"))
				return true;
			std::string body;
			std::getline(input >> std::ws, body);
			if (body.empty())
			{
				m_permissions.audit(actor, "announce", "all", "failed", "missing text");
				send_system_message(player, "Verwendung: /announce <Text>", now, protocol::CHAT_CHANNEL_ADMIN);
				return true;
			}
			protocol::Envelope envelope;
			auto *chat = envelope.mutable_chat_broadcast();
			chat->set_player_id(0);
			chat->set_display_name("Spielleitung");
			chat->set_text(body);
			chat->set_server_time_ms(milliseconds(now));
			chat->set_channel(protocol::CHAT_CHANNEL_ANNOUNCEMENT);
			broadcast(std::move(envelope), reliability::reliable);
			m_permissions.audit(actor, "announce", "all", "allowed", body);
			return true;
		}

		player_id target_id{};
		if (command == "/kick")
		{
			if (!require("admin.kick", "player.kick"))
				return true;
			input >> target_id;
			auto *target = parse_target(target_id);
			std::string reason;
			std::getline(input >> std::ws, reason);
			if (!target || target->dummy)
			{
				m_permissions.audit(actor, "player.kick", std::to_string(target_id), "failed", "unknown player");
				send_system_message(player, "Unbekannte Spieler-ID.", now, protocol::CHAT_CHANNEL_ADMIN);
				return true;
			}
			m_permissions.audit(actor, "player.kick", target->profile.persistent_id(), "allowed", reason);
			kick(target_id, reason.empty() ? "Von der Spielleitung entfernt" : reason, now);
			return true;
		}
		if (command == "/goto" || command == "/bring")
		{
			if (!require("admin.teleport", command == "/goto" ? "player.goto" : "player.bring"))
				return true;
			input >> target_id;
			auto *target = parse_target(target_id);
			if (!target || !target->has_transform || !player.has_transform)
			{
				m_permissions.audit(actor, command == "/goto" ? "player.goto" : "player.bring", std::to_string(target_id), "failed", "position unavailable");
				send_system_message(player, "Spieler oder Position nicht verfuegbar.", now, protocol::CHAT_CHANNEL_ADMIN);
				return true;
			}
			auto &moving = command == "/goto" ? player : *target;
			const auto &destination = command == "/goto" ? target->transform : player.transform;
			const auto accepted = teleport_player(moving, destination, "Teleport durch Spielleitung", now);
			m_permissions.audit(actor, command == "/goto" ? "player.goto" : "player.bring", target->profile.persistent_id(), accepted ? "allowed" : "failed");
			return true;
		}
		if (command == "/freeze" || command == "/unfreeze")
		{
			if (!require("admin.freeze", command == "/freeze" ? "player.freeze" : "player.unfreeze"))
				return true;
			input >> target_id;
			auto *target = parse_target(target_id);
			if (!target || !target->connection || !target->has_transform)
			{
				m_permissions.audit(actor, command == "/freeze" ? "player.freeze" : "player.unfreeze", std::to_string(target_id), "failed", "position unavailable");
				send_system_message(player, "Spieler oder Position nicht verfuegbar.", now, protocol::CHAT_CHANNEL_ADMIN);
				return true;
			}
			target->frozen = command == "/freeze";
			if (target->frozen)
				target->frozen_transform = target->transform;
			m_permissions.audit(actor, target->frozen ? "player.freeze" : "player.unfreeze", target->profile.persistent_id(), "allowed");
			send_system_message(player, target->display_name + (target->frozen ? " wurde eingefroren." : " wurde freigegeben."), now, protocol::CHAT_CHANNEL_ADMIN);
			return true;
		}
		if (command == "/perm")
		{
			if (!require("admin.permissions", "permission.manage"))
				return true;
			std::string action;
			std::string scope;
			input >> action >> target_id >> scope;
			action = lower_ascii(action);
			auto *target = parse_target(target_id);
			if (!target || target->dummy || (action != "list" && scope.empty()))
			{
				m_permissions.audit(actor, "permission." + action, std::to_string(target_id), "failed", "invalid arguments");
				send_system_message(player, "Verwendung: /perm <list|grant|revoke> <Spieler-ID> [Scope]", now, protocol::CHAT_CHANNEL_ADMIN);
				return true;
			}
			const auto target_persistent = target->profile.persistent_id();
			if (action == "list")
			{
				auto scopes = m_permissions.list(target_persistent);
				std::string joined = scopes.empty() ? "(keine)" : scopes.front();
				for (std::size_t index = 1; index < scopes.size(); ++index)
					joined += ", " + scopes[index];
				send_system_message(player, target->display_name + ": " + joined, now, protocol::CHAT_CHANNEL_ADMIN);
				m_permissions.audit(actor, "permission.list", target_persistent, "allowed");
				return true;
			}
			std::string error;
			const auto accepted = action == "grant"
			    ? m_permissions.grant(target_persistent, scope, error)
			    : action == "revoke"
			        ? m_permissions.revoke(target_persistent, scope, error)
			        : false;
			if (action != "grant" && action != "revoke")
				error = "unbekannte Aktion";
			m_permissions.audit(actor, "permission." + action, target_persistent, accepted ? "allowed" : "failed", scope);
			send_system_message(player, accepted ? "Berechtigungen aktualisiert." : "Fehler: " + error, now, protocol::CHAT_CHANNEL_ADMIN);
			return true;
		}

		m_permissions.audit(actor, "admin.unknown", "", "failed", command);
		send_system_message(player, "Unbekannter Befehl. /adminhelp zeigt GM-Befehle.", now, protocol::CHAT_CHANNEL_ADMIN);
		return false;
	}

	void server_core::handle_ping(
	    player_session &player,
	    const protocol::Ping &message,
	    time_point now)
	{
		protocol::Envelope envelope;
		auto *pong = envelope.mutable_pong();
		pong->set_nonce(message.nonce());
		pong->set_client_time_ms(message.client_time_ms());
		pong->set_server_time_ms(milliseconds(now));
		queue(*player.connection, std::move(envelope), reliability::reliable);
	}

	std::uint32_t server_core::effective_sleep_requirement() const
	{
		const auto eligible = static_cast<std::uint32_t>(std::ranges::count_if(
		    m_players,
		    [](const auto &entry)
		    {
			    const auto &player = entry.second;
			    return !player.dummy && player.connection.has_value()
			        && !player.dead;
		    }));
		return eligible == 0
		    ? 1U
		    : std::min(m_config.sleeping_players_required, eligible);
	}

	void server_core::broadcast_sleep_state(bool time_skipped)
	{
		protocol::Envelope envelope;
		auto *state = envelope.mutable_server_sleep_state();
		state->set_revision(m_sleep_revision);
		state->set_sleeping_players(static_cast<std::uint32_t>(
		    m_sleeping_players.size()));
		state->set_required_players(effective_sleep_requirement());
		state->set_time_skipped(time_skipped);
		broadcast(std::move(envelope), reliability::reliable);
	}

	void server_core::remove_sleep_vote(player_id id)
	{
		if (m_sleeping_players.erase(id) == 0)
			return;
		++m_sleep_revision;
		broadcast_sleep_state();
	}

	void server_core::handle_sleep_state(
	    player_session &player,
	    const protocol::ClientSleepState &message,
	    time_point now)
	{
		if (player.dead)
			return;
		const bool changed = message.sleeping()
		    ? m_sleeping_players.insert(player.id).second
		    : m_sleeping_players.erase(player.id) != 0;
		if (!changed)
			return;
		++m_sleep_revision;
		if (message.sleeping()
		    && m_sleeping_players.size() >= effective_sleep_requirement())
		{
			advance_environment_clock(now);
			m_environment_anchor_world_seconds = next_world_time_at_hour(
			    current_environment(now).world_time_seconds(),
			    m_config.sleep_wake_hour);
			m_environment_anchor_time = now;
			++m_environment_revision;
			broadcast_environment(now);
			m_sleeping_players.clear();
			++m_sleep_revision;
			broadcast_sleep_state(true);
			broadcast_system_message(
			    "Time advanced because enough players went to sleep.",
			    now);
			return;
		}
		broadcast_sleep_state();
	}

	void server_core::handle_death(player_session &player, time_point now)
	{
		if (player.dead)
			return;
		player.dead = true;
		remove_sleep_vote(player.id);
		release_activity(player);
		broadcast_system_message(player.display_name + " died.", now);
	}

	void server_core::handle_respawn_request(
	    player_session &player,
	    time_point now)
	{
		if (!player.dead || !player.connection
		    || !m_store.manifest().spawn_valid)
			return;
		auto spawn = m_store.manifest().spawn;
		spawn.set_sequence(std::max(
		    spawn.sequence(),
		    player.last_sequence + 1));
		spawn.set_client_time_ms(milliseconds(now));
		player.dead = false;
		player.has_transform = true;
		player.transform = spawn;
		player.last_sequence = spawn.sequence();
		player.last_transform_at = now;
		player.profile.set_transform_valid(true);
		*player.profile.mutable_last_transform() = spawn;
		persist_player(player, now);

		protocol::Envelope envelope;
		*envelope.mutable_server_respawn()->mutable_spawn() = spawn;
		queue(*player.connection, std::move(envelope), reliability::reliable);
		++m_sleep_revision;
		broadcast_sleep_state();
		broadcast_system_message(player.display_name + " respawned.", now);
	}

	void server_core::handle_activity_start(
	    player_session &player,
	    const protocol::ClientActivityStart &message)
	{
		auto deny = [&](std::string reason)
		{
			if (!player.connection)
				return;
			protocol::Envelope envelope;
			auto *denied = envelope.mutable_activity_denied();
			denied->set_kind(message.kind());
			denied->set_station_guid(message.station_guid());
			denied->set_reason(std::move(reason));
			queue(
			    *player.connection,
			    std::move(envelope),
			    reliability::reliable);
		};

		if (player.dead)
		{
			deny("dead players cannot use activity stations");
			return;
		}
		if (player.activity.active())
		{
			deny("player already owns an activity session");
			return;
		}
		if (const auto occupied = m_station_owners.find(message.station_guid());
		    occupied != m_station_owners.end() && occupied->second != player.id)
		{
			deny("activity station is already in use");
			return;
		}

		auto session_id = m_next_activity_session_id++;
		if (session_id == 0)
			session_id = m_next_activity_session_id++;
		auto activity = player.activity;
		activity.set_kind(message.kind());
		activity.set_station_guid(message.station_guid());
		activity.set_session_id(session_id);
		activity.set_revision(activity.revision() + 1);
		activity.set_active(true);
		player.activity = activity;
		m_station_owners.insert_or_assign(message.station_guid(), player.id);

		if (player.connection)
		{
			protocol::Envelope granted;
			*granted.mutable_activity_granted()->mutable_activity() = activity;
			queue(
			    *player.connection,
			    std::move(granted),
			    reliability::reliable);
		}
		protocol::Envelope updated;
		auto *broadcast_update = updated.mutable_player_activity_updated();
		broadcast_update->set_player_id(player.id);
		*broadcast_update->mutable_activity() = activity;
		broadcast(
		    std::move(updated),
		    reliability::reliable,
		    player.connection);
	}

	void server_core::handle_activity_end(
	    player_session &player,
	    const protocol::ClientActivityEnd &message)
	{
		if (!player.activity.active()
		    || player.activity.session_id() != message.session_id())
		{
			return;
		}
		release_activity(
		    player,
		    message.has_final_transform() ? &message.final_transform() : nullptr);
	}

	void server_core::release_activity(
	    player_session &player,
	    const protocol::TransformState *final_transform)
	{
		if (!player.activity.active())
			return;

		if (const auto owner = m_station_owners.find(
		        player.activity.station_guid());
		    owner != m_station_owners.end() && owner->second == player.id)
		{
			m_station_owners.erase(owner);
		}
		auto ended = player.activity;
		ended.set_active(false);
		ended.set_revision(ended.revision() + 1);
		player.activity = ended;

		protocol::Envelope updated;
		auto *broadcast_update = updated.mutable_player_activity_updated();
		broadcast_update->set_player_id(player.id);
		*broadcast_update->mutable_activity() = ended;
		if (final_transform)
			*broadcast_update->mutable_final_transform() = *final_transform;
		broadcast(std::move(updated), reliability::reliable);
	}

	void server_core::reject(
	    connection_id connection,
	    protocol::RejectReason reason,
	    std::string message)
	{
		release_initializer(connection);
		m_pending.erase(connection);
		protocol::Envelope envelope;
		auto *rejected = envelope.mutable_server_rejected();
		rejected->set_reason(reason);
		rejected->set_message(std::move(message));
		queue(
		    connection,
		    std::move(envelope),
		    reliability::reliable,
		    close_kind::reject);
		wake_bootstrap_waiters();
	}

	void server_core::remove_player(
	    player_id id,
	    std::string reason,
	    close_kind close,
	    time_point now)
	{
		const auto iterator = m_players.find(id);
		if (iterator == m_players.end())
		{
			return;
		}
		const auto dummy = iterator->second.dummy;
		const auto display_name = iterator->second.display_name;
		persist_player(iterator->second, now);
		remove_sleep_vote(id);
		release_activity(iterator->second);
		const auto connection = iterator->second.connection;
		if (connection && close != close_kind::none)
		{
			queue(
			    *connection,
			    player_left_envelope(id, reason),
			    reliability::reliable,
			    close);
		}
		m_players.erase(iterator);
		m_npc_delivery.erase(id);
		const auto positions = player_positions();
		queue_npc_events(m_npcs.remove_player(id, positions, now));
		if (dummy)
			m_items.erase_location(item_location::player(id));
		broadcast(
		    player_left_envelope(id, reason),
		    reliability::reliable,
		    connection);
		if (!dummy)
		{
			const auto notice = close == close_kind::kick
			    ? display_name
			        + (reason == "timed out" ? " timed out."
			                                 : " was kicked from the server.")
			    : display_name + " left the server.";
			broadcast_system_message(notice, now, connection);
		}
	}

	void server_core::send_accepted(player_session &player)
	{
		protocol::Envelope envelope;
		auto *accepted = envelope.mutable_server_accepted();
		accepted->set_player_id(player.id);
		accepted->set_resume_token(player.resume_token);
		accepted->set_tick_rate(m_config.tick_rate);
		accepted->set_snapshot_rate(m_config.snapshot_rate);
		accepted->set_max_players(m_config.max_players);
		accepted->set_server_name(m_config.name);
		accepted->set_level_id(m_store.manifest().level_id);
		accepted->set_network_role(network_role(player.network_role));
		accepted->set_profile_snapshot_interval_seconds(
		    m_config.profile_snapshot_interval_seconds);
		*accepted->mutable_avatar_policy() = avatar_policy();
		if (const auto marker = m_properties.home_marker_for(
		        player.profile.persistent_id(), unix_milliseconds()))
			*accepted->mutable_home_marker() = *marker;
		for (const auto &[id, session] : m_players)
		{
			(void)id;
			*accepted->add_players() = snapshot_of(session, true);
		}
		queue(*player.connection, std::move(envelope), reliability::reliable);
		send_entity_control(*player.connection);
		protocol::Envelope sleep;
		auto *sleep_state = sleep.mutable_server_sleep_state();
		sleep_state->set_revision(m_sleep_revision);
		sleep_state->set_sleeping_players(static_cast<std::uint32_t>(
		    m_sleeping_players.size()));
		sleep_state->set_required_players(effective_sleep_requirement());
		queue(*player.connection, std::move(sleep), reliability::reliable);
	}

	void server_core::broadcast_home_markers()
	{
		const auto now = unix_milliseconds();
		for (const auto &[id, player] : m_players)
		{
			(void)id;
			if (!player.connection)
				continue;
			protocol::Envelope envelope;
			auto *updated = envelope.mutable_server_home_marker_updated();
			updated->set_ledger_revision(m_properties.ledger().revision());
			if (const auto marker = m_properties.home_marker_for(
			        player.profile.persistent_id(), now))
			{
				updated->set_active(true);
				*updated->mutable_marker() = *marker;
			}
			queue(
			    *player.connection,
			    std::move(envelope),
			    reliability::reliable);
		}
	}

	void server_core::send_entity_control(connection_id connection)
	{
		protocol::Envelope envelope;
		auto *control = envelope.mutable_server_entity_control();
		control->set_non_player_entities_disabled(
		    m_human_npcs_disabled && m_animal_npcs_disabled);
		control->set_human_npcs_disabled(m_human_npcs_disabled);
		control->set_animal_npcs_disabled(m_animal_npcs_disabled);
		queue(connection, std::move(envelope), reliability::reliable);
	}

	void server_core::send_world_objects(connection_id connection)
	{
		for (const auto &[guid, object] : m_world_objects)
		{
			(void)guid;
			protocol::Envelope envelope;
			*envelope.mutable_world_object_updated()->mutable_state() = object;
			queue(connection, std::move(envelope), reliability::reliable);
		}
	}

	void server_core::send_world_items(connection_id connection)
	{
		for (const auto &[instance, item] : m_world_items)
		{
			(void)instance;
			protocol::Envelope envelope;
			*envelope.mutable_world_item_updated()->mutable_state() = item;
			queue(connection, std::move(envelope), reliability::reliable);
		}
	}

	void server_core::advance_environment_clock(time_point now)
	{
		m_current_time = now;
		if (!m_environment_clock_started)
		{
			m_environment_anchor_time = now;
			m_environment_clock_started = true;
		}
	}

	void server_core::broadcast_environment(time_point now)
	{
		protocol::Envelope envelope;
		*envelope.mutable_server_environment_updated()->mutable_state() =
		    current_environment(now);
		broadcast(std::move(envelope), reliability::reliable);
	}

	void server_core::apply_default_avatar(
	    protocol::PlayerProfile &profile)
	{
		auto *avatar = profile.mutable_avatar();
		avatar->Clear();
		avatar->set_archetype_id(m_config.default_avatar_archetype);
		avatar->set_revision(1);
		avatar->set_stance(protocol::AVATAR_STANCE_RELAXED);
		avatar->set_weapon_class(protocol::AVATAR_WEAPON_CLASS_NONE);
		avatar->set_weapon_drawn(false);
		for (const auto &item : profile.inventory())
		{
			if (!item.has_equipped_slot())
				continue;
			auto *visible = avatar->add_equipment();
			visible->set_definition_id(item.definition_id());
			visible->set_equipped_slot(item.equipped_slot());
		}
	}

	bool server_core::avatar_allowed(
	    const protocol::AvatarDescriptor &avatar) const
	{
		return is_valid_avatar_descriptor(avatar)
		    && std::ranges::find(
		           m_config.allowed_avatar_archetypes,
		           avatar.archetype_id())
		        != m_config.allowed_avatar_archetypes.end();
	}

	protocol::AvatarPolicy server_core::avatar_policy() const
	{
		protocol::AvatarPolicy result;
		result.set_default_archetype_id(
		    m_config.default_avatar_archetype);
		for (const auto &archetype :
		     m_config.allowed_avatar_archetypes)
		{
			result.add_allowed_archetype_ids(archetype);
		}
		return result;
	}

	void server_core::send_challenge(
	    connection_id connection,
	    std::uint64_t client_features)
	{
		protocol::Envelope envelope;
		auto *challenge = envelope.mutable_server_challenge();
		challenge->set_server_id(m_config.account_auth_enabled
		    ? m_config.account_server_id
		    : m_store.manifest().server_id);
		challenge->set_central_auth_required(m_config.account_auth_enabled);
		challenge->set_required_runtime_features(
		    required_client_runtime_capabilities);
		challenge->set_negotiated_runtime_features(
		    negotiate_runtime_capabilities(client_features));
		queue(connection, std::move(envelope), reliability::reliable);
	}

	void server_core::send_bootstrap(
	    connection_id connection,
	    protocol::BootstrapMode mode)
	{
		auto &pending = m_pending.at(connection);
		protocol::Envelope envelope;
		auto *bootstrap = envelope.mutable_server_bootstrap();
		bootstrap->set_server_id(m_store.manifest().server_id);
		bootstrap->set_session_id(m_store.manifest().session_id);
		bootstrap->set_manifest_revision(m_store.manifest().revision);
		bootstrap->set_level_id(m_store.manifest().level_id);
		bootstrap->set_world_seed(m_store.manifest().world_seed);
		bootstrap->set_mode(mode);
		bootstrap->set_spawn_valid(m_store.manifest().spawn_valid);
		bootstrap->set_timeout_seconds(m_config.bootstrap_timeout_seconds);
		bootstrap->set_issued_identity_token(
		    pending.issued_identity_token);
		*bootstrap->mutable_environment() = current_environment(m_current_time);
		if (m_store.manifest().spawn_valid)
		{
			*bootstrap->mutable_spawn() = m_store.manifest().spawn;
		}
		if (pending.persisted)
		{
			*bootstrap->mutable_profile() = pending.persisted->profile;
		}
		queue(connection, std::move(envelope), reliability::reliable);
	}

	void server_core::release_initializer(connection_id connection)
	{
		if (m_initializer == connection)
		{
			m_initializer.reset();
		}
	}

	void server_core::wake_bootstrap_waiters()
	{
		if (m_store.manifest().spawn_valid)
		{
			for (auto &[connection, pending] : m_pending)
			{
				if (pending.stage == pending_stage::waiting_for_initializer)
				{
					pending.stage = pending_stage::loading_world;
					send_bootstrap(connection, protocol::BOOTSTRAP_MODE_LOAD);
				}
			}
			return;
		}
		if (m_initializer)
		{
			return;
		}
		const auto waiter = std::ranges::find_if(
		    m_pending,
		    [](const auto &entry)
		    {
			    return entry.second.stage
			        == pending_stage::waiting_for_initializer;
		    });
		if (waiter != m_pending.end())
		{
			m_initializer = waiter->first;
			waiter->second.initializer = true;
			waiter->second.stage = pending_stage::loading_world;
			send_bootstrap(
			    waiter->first,
			    protocol::BOOTSTRAP_MODE_INITIALIZE);
		}
	}

	void server_core::persist_player(player_session &player, time_point now)
	{
		if (player.dummy)
		{
			return;
		}
		player.profile.set_player_id(player.id);
		player.profile.set_display_name(player.display_name);
		player.profile.set_level_id(m_store.manifest().level_id);
		player.profile.set_transform_valid(player.has_transform);
		if (player.has_transform)
		{
			*player.profile.mutable_last_transform() = player.transform;
		}
		*player.profile.mutable_avatar() = player.avatar;
		m_store.save_profile(player.identity_hash, player.profile);
		player.last_persisted_at = now;
	}

	void server_core::persist_world_objects()
	{
		std::vector<protocol::WorldObjectState> objects;
		objects.reserve(m_world_objects.size());
		for (const auto &[guid, object] : m_world_objects)
		{
			(void)guid;
			objects.push_back(object);
		}
		std::ranges::sort(
		    objects,
		    {},
		    &protocol::WorldObjectState::entity_guid);
		m_store.save_world_objects(objects);
	}

	void server_core::persist_world_items()
	{
		std::vector<protocol::WorldItemState> items;
		items.reserve(m_world_items.size());
		for (const auto &[instance, item] : m_world_items)
		{
			(void)instance;
			items.push_back(item);
		}
		std::ranges::sort(items, {}, &protocol::WorldItemState::instance_id);
		m_store.save_world_items(items);
	}

	void server_core::rebuild_avatar_equipment(player_session &player)
	{
		auto *equipment = player.profile.mutable_avatar()->mutable_equipment();
		equipment->Clear();
		for (const auto &item : player.profile.inventory())
		{
			if (!item.has_equipped_slot())
				continue;
			auto *visible = equipment->Add();
			visible->set_definition_id(item.definition_id());
			visible->set_equipped_slot(item.equipped_slot());
		}
		player.avatar = player.profile.avatar();
	}

	void server_core::remove_owned_items_from_world()
	{
		bool changed = false;
		for (auto &[guid, object] : m_world_objects)
		{
			(void)guid;
			auto *inventory = object.mutable_inventory();
			bool object_changed = false;
			for (auto index = inventory->size(); index-- > 0;)
			{
				const auto *entry =
				    m_items.find(inventory->Get(index).instance_id());
				if (entry
				    && entry->location.kind == item_location_kind::player)
				{
					inventory->DeleteSubrange(index, 1);
					object_changed = true;
				}
			}
			if (!object_changed)
				continue;

			object.set_revision(object.revision() + 1);
			changed = true;
			protocol::Envelope updated;
			*updated.mutable_world_object_updated()->mutable_state() = object;
			broadcast(std::move(updated), reliability::reliable);
		}
		if (changed)
			persist_world_objects();

		bool items_changed = false;
		for (auto &[instance, item] : m_world_items)
		{
			const auto *entry = m_items.find(instance);
			if (!item.present() || !entry
			    || entry->location.kind != item_location_kind::player)
				continue;
			item.set_present(false);
			item.set_revision(item.revision() + 1);
			items_changed = true;
			protocol::Envelope updated;
			*updated.mutable_world_item_updated()->mutable_state() = item;
			broadcast(std::move(updated), reliability::reliable);
		}
		if (items_changed)
			persist_world_items();
	}

	void server_core::broadcast(
	    protocol::Envelope envelope,
	    reliability delivery,
	    std::optional<connection_id> except)
	{
		for (const auto &[id, player] : m_players)
		{
			(void)id;
			if (!player.connection || player.connection == except)
			{
				continue;
			}
			queue(*player.connection, envelope, delivery);
		}
	}

	void server_core::queue(
	    connection_id connection,
	    protocol::Envelope envelope,
	    reliability delivery,
	    close_kind close)
	{
		outbound_message outgoing{
		    connection, std::move(envelope), delivery, close};
		if (outgoing.envelope.has_chat_broadcast())
		{
			const auto first_sync = std::ranges::find_if(
			    m_outbound,
			    [](const outbound_message &queued)
			    {
				    return !queued.envelope.has_chat_broadcast();
			    });
			m_outbound.insert(first_sync, std::move(outgoing));
		}
		else
		{
			m_outbound.push_back(std::move(outgoing));
		}
	}

	void server_core::queue_snapshot(time_point now)
	{
		if (m_players.empty())
		{
			return;
		}
		protocol::Envelope envelope;
		auto *snapshot = envelope.mutable_world_snapshot();
		snapshot->set_server_tick(m_server_tick);
		snapshot->set_server_time_ms(milliseconds(now));
		*snapshot->mutable_environment() = current_environment(now);
		for (const auto &[id, player] : m_players)
		{
			(void)id;
			*snapshot->add_players() = snapshot_of(player, false);
		}
		broadcast(std::move(envelope), reliability::unreliable);
		queue_npc_snapshots();
	}

	std::vector<npc_registry::player_position>
	server_core::player_positions() const
	{
		std::vector<npc_registry::player_position> result;
		result.reserve(m_players.size());
		for (const auto &[id, player] : m_players)
		{
			if (player.dummy)
				continue;
			result.push_back({
			    id,
			    player.has_transform ? &player.transform : nullptr,
			    player.connection.has_value()});
		}
		return result;
	}

	void server_core::queue_npc_events(
	    std::vector<npc_registry::event> events)
	{
		for (auto &event : events)
		{
			const auto player = m_players.find(event.recipient);
			if (player == m_players.end() || !player->second.connection)
				continue;
			protocol::Envelope envelope;
			switch (event.kind)
			{
			case npc_registry::event_kind::enter:
			{
				auto &delivery = m_npc_delivery[event.recipient]
				    [event.state.npc_id()];
				delivery.motion_revision = event.state.revision();
				delivery.gameplay_revision = event.state.has_gameplay()
				    ? event.state.gameplay().revision() : 0;
				delivery.inventory_revision =
				    event.state.has_gameplay()
				        && event.state.gameplay().has_inventory()
				    ? event.state.gameplay().inventory().revision() : 0;
				delivery.motion_sent_at = m_current_time;
				*envelope.mutable_server_npc_enter()->mutable_state() =
				    std::move(event.state);
				break;
			}
			case npc_registry::event_kind::leave:
				m_npc_delivery[event.recipient].erase(event.state.npc_id());
				envelope.mutable_server_npc_leave()->set_npc_id(
				    event.state.npc_id());
				envelope.mutable_server_npc_leave()->set_generation(
				    event.state.generation());
				break;
			case npc_registry::event_kind::authority:
			{
				auto *authority = envelope.mutable_server_npc_authority();
				authority->set_npc_id(event.state.npc_id());
				authority->set_generation(event.state.generation());
				authority->set_authority_player_id(
				    event.state.authority_player_id());
				authority->set_lease_id(event.state.lease_id());
				break;
			}
			}
			queue(
			    *player->second.connection,
			    std::move(envelope),
			    reliability::reliable);
		}
	}

	void server_core::queue_npc_snapshots()
	{
		constexpr std::size_t npc_motion_tick_budget = 12 * 1024;
		constexpr auto motion_keyframe_interval = std::chrono::seconds(2);
		const auto now = m_current_time == time_point{}
		    ? std::chrono::steady_clock::now() : m_current_time;
		for (const auto &[id, player] : m_players)
		{
			if (!player.connection || player.dummy)
				continue;
			auto states = m_npcs.states_for(id);
			auto &deliveries = m_npc_delivery[id];
			for (const auto &state : states)
				deliveries.try_emplace(state.npc_id());

			// Gameplay revisions are sparse and must not depend on the lossy motion
			// stream. Enter already carries a full baseline; later changes are sent
			// reliably, with inventory omitted unless its own revision changed.
			for (const auto &state : states)
			{
				if (!state.has_gameplay())
					continue;
				auto &delivery = deliveries[state.npc_id()];
				if (state.gameplay().revision() <= delivery.gameplay_revision)
					continue;

				protocol::Envelope envelope;
				auto *update = envelope.mutable_server_npc_gameplay_update();
				update->set_npc_id(state.npc_id());
				update->set_generation(state.generation());
				update->set_state_revision(state.revision());
				*update->mutable_gameplay() = state.gameplay();
				if (update->gameplay().has_inventory()
				    && update->gameplay().inventory().revision()
				        == delivery.inventory_revision)
					update->mutable_gameplay()->clear_inventory();
				else if (update->gameplay().has_inventory())
					delivery.inventory_revision =
					    update->gameplay().inventory().revision();

				queue(
				    *player.connection,
				    std::move(envelope),
				    reliability::reliable);
				delivery.gameplay_revision = state.gameplay().revision();
			}

			std::ranges::sort(
			    states,
			    [&](const protocol::NpcState &left,
			        const protocol::NpcState &right)
			    {
				const auto &a = deliveries.at(left.npc_id());
				const auto &b = deliveries.at(right.npc_id());
				return a.motion_sent_at == b.motion_sent_at
				    ? left.npc_id() < right.npc_id()
				    : a.motion_sent_at < b.motion_sent_at;
			    });

			protocol::Envelope envelope;
			auto *motion = envelope.mutable_server_npc_motion();
			motion->set_server_tick(m_server_tick);
			std::size_t payload_size = 32;
			for (const auto &state : states)
			{
				auto &delivery = deliveries[state.npc_id()];
				const bool keyframe_due = delivery.motion_sent_at == time_point{}
				    || now - delivery.motion_sent_at >= motion_keyframe_interval;
				if (state.revision() <= delivery.motion_revision && !keyframe_due)
					continue;

				protocol::NpcMotionState candidate;
				candidate.set_npc_id(state.npc_id());
				candidate.set_generation(state.generation());
				candidate.set_revision(state.revision());
				*candidate.mutable_transform() = state.transform();
				const auto candidate_size = candidate.ByteSizeLong() + 16;
				if (motion->npcs_size() != 0
				    && (motion->npcs_size()
				            >= static_cast<int>(max_npcs_per_message)
				        || payload_size + candidate_size > npc_motion_tick_budget))
					break;

				*motion->add_npcs() = std::move(candidate);
				payload_size += candidate_size;
				delivery.motion_revision = state.revision();
				delivery.motion_sent_at = now;
			}
			if (motion->npcs_size() != 0)
				queue(
				    *player.connection,
				    std::move(envelope),
				    reliability::unreliable);
		}
	}

	server_core::player_session *server_core::find_by_connection(
	    connection_id connection)
	{
		for (auto &[id, player] : m_players)
		{
			(void)id;
			if (player.connection == connection)
			{
				return &player;
			}
		}
		return nullptr;
	}

	server_core::player_session *server_core::find_by_resume_token(
	    std::string_view token)
	{
		for (auto &[id, player] : m_players)
		{
			(void)id;
			if (player.resume_token == token)
			{
				return &player;
			}
		}
		return nullptr;
	}

	std::string server_core::lower_ascii(std::string_view value)
	{
		return lowercase_ascii(value);
	}

	std::uint64_t server_core::milliseconds(time_point value)
	{
		return static_cast<std::uint64_t>(
		    std::chrono::duration_cast<std::chrono::milliseconds>(
		        value.time_since_epoch())
		        .count());
	}

	protocol::PlayerSnapshot server_core::snapshot_of(
	    const player_session &player,
	    bool include_avatar)
	{
		protocol::PlayerSnapshot snapshot;
		snapshot.set_player_id(player.id);
		if (!player.profile.persistent_id().empty())
			snapshot.set_persistent_id(player.profile.persistent_id());
		snapshot.set_display_name(player.display_name);
		snapshot.set_network_role(network_role(player.network_role));
		snapshot.set_transform_valid(player.has_transform);
		snapshot.set_connected(
		    player.dummy || player.connection.has_value());
		snapshot.set_movement_mode(player.movement_mode);
		if (player.has_transform)
		{
			*snapshot.mutable_transform() = player.transform;
		}
		if (include_avatar)
		{
			*snapshot.mutable_avatar() = player.avatar;
		}
		if (player.activity.active())
			*snapshot.mutable_activity() = player.activity;
		return snapshot;
	}
}
