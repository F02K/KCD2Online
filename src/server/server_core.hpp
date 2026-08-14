#pragma once

#include "multiplayer/protocol.hpp"
#include "property/service.hpp"
#include "server/item_ledger.hpp"
#include "server/network_identity.hpp"
#include "server/npc_registry.hpp"
#include "server/permission_store.hpp"
#include "server/server_config.hpp"
#include "server/world_store.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kcd2o::server
{
	using clock = std::chrono::steady_clock;
	using time_point = clock::time_point;

	enum class close_kind
	{
		none,
		reject,
		shutdown,
		kick
	};

	struct outbound_message
	{
		connection_id connection{};
		protocol::Envelope envelope;
		reliability delivery{reliability::reliable};
		close_kind close_after_send{close_kind::none};
	};

	struct player_view
	{
		player_id id{};
		std::string persistent_id;
		std::string display_name;
		bool connected{};
		bool has_transform{};
		std::uint64_t last_sequence{};
		protocol::MovementMode movement_mode{protocol::MOVEMENT_MODE_IDLE};
		bool dummy{};
		std::string network_role{"user"};
	};

	class server_core
	{
	public:
		using token_generator = std::function<std::string()>;
		using account_authenticator = std::function<std::optional<network_identity>(
		    std::string_view,
		    authentication_failure &)>;
		using moderation_executor = std::function<bool(
		    const moderation_action &,
		    std::string &)>;

		explicit server_core(
		    server_config config,
		    token_generator generate_token = {},
		    account_authenticator authenticate_account = {},
		    moderation_executor moderate_account = {});

		void on_transport_connected(connection_id connection, time_point now);
		void on_transport_disconnected(
		    connection_id connection,
		    bool allow_reconnect,
		    std::string reason,
		    time_point now);
		void on_message(
		    connection_id connection,
		    const protocol::Envelope &envelope,
		    time_point now);
		void tick(time_point now);
		void kick(player_id id, std::string reason, time_point now);
		[[nodiscard]] std::optional<player_id> spawn_dummy(
		    std::string display_name,
		    std::string *error = nullptr);
		[[nodiscard]] bool remove_dummy(player_id id, time_point now);
		void server_say(std::string text, time_point now);
		[[nodiscard]] bool grant_permission(
		    player_id id,
		    std::string permission,
		    std::string &error);
		[[nodiscard]] bool revoke_permission(
		    player_id id,
		    std::string_view permission,
		    std::string &error);
		[[nodiscard]] std::vector<std::string> permissions(player_id id) const;
		[[nodiscard]] bool set_npc_entities_disabled(
		    bool humans_disabled,
		    bool animals_disabled);
		[[nodiscard]] bool human_npcs_disabled() const;
		[[nodiscard]] bool animal_npcs_disabled() const;
		[[nodiscard]] protocol::EnvironmentState current_environment(
		    time_point now) const;
		[[nodiscard]] bool set_time_of_day(double hours, time_point now);
		[[nodiscard]] bool set_time_scale(float scale, time_point now);
		[[nodiscard]] bool set_weather(
		    std::uint32_t weather_id,
		    std::uint32_t transition_seconds,
		    time_point now);
		void shutdown(std::string reason);
		[[nodiscard]] std::optional<std::string> create_profile_claim(
		    player_id id,
		    time_point now);
		[[nodiscard]] const protocol::PropertyCatalog &property_catalog() const;
		[[nodiscard]] const protocol::PropertyLedger &property_ledger() const;
		[[nodiscard]] bool assign_property_owner(
		    std::string_view property_id,
		    player_id target,
		    std::string &error);
		[[nodiscard]] bool grant_property_role(
		    player_id actor,
		    std::string_view property_id,
		    player_id target,
		    protocol::PropertyRole role,
		    std::uint64_t expires_at_ms,
		    std::string &error);
		[[nodiscard]] bool revoke_property_role(
		    player_id actor,
		    std::string_view assignment_id,
		    std::string &error);
		[[nodiscard]] bool system_revoke_property_role(
		    std::string_view assignment_id,
		    std::string &error);

		[[nodiscard]] std::vector<outbound_message> take_outbound();
		[[nodiscard]] std::vector<player_view> players() const;
		[[nodiscard]] std::size_t pending_connection_count() const;
		[[nodiscard]] std::uint64_t server_tick() const;
		[[nodiscard]] const server_config &config() const;
		void apply_account_restrictions(
		    const std::vector<account_restriction> &restrictions,
		    time_point now);
		void apply_moderation_action(
		    const moderation_action &action,
		    time_point now);

	private:
		enum class pending_stage
		{
			hello,
			authenticate,
			waiting_for_initializer,
			loading_world
		};

		struct pending_connection
		{
			time_point connected_at;
			time_point deadline;
			pending_stage stage{pending_stage::hello};
			std::string display_name;
			std::string content_hash;
			bool password_accepted{};
			std::optional<network_identity> network;
			std::optional<persisted_profile> persisted;
			std::string issued_identity_token;
			std::string resume_token;
			bool initializer{};
		};

		struct player_session
		{
			player_id id{};
			std::string display_name;
			std::string resume_token;
			token_hash identity_hash{};
			std::optional<connection_id> connection;
			bool dummy{};
			std::string network_role{"user"};
			bool network_full_permissions{};
			bool network_chat_muted{};
			bool network_voice_muted{};
			bool has_transform{};
			protocol::TransformState transform;
			protocol::MovementMode movement_mode{protocol::MOVEMENT_MODE_IDLE};
			std::uint64_t last_sequence{};
			time_point last_message_at;
			time_point last_transform_at;
			time_point reconnect_deadline;
			std::deque<time_point> chat_times;
			std::deque<time_point> avatar_update_times;
			std::deque<time_point> voice_frame_times;
			protocol::AvatarDescriptor avatar;
			protocol::PlayerProfile profile;
			time_point last_persisted_at;
			bool dead{};
			bool frozen{};
			protocol::TransformState frozen_transform;
			protocol::PlayerActivity activity;
			std::uint64_t dummy_random_state{};
			time_point dummy_last_update;
			time_point dummy_action_ends_at;
			time_point dummy_next_input_at;
		};

		struct profile_claim
		{
			token_hash code_hash{};
			time_point expires_at;
		};

		void handle_hello(
		    connection_id connection,
		    const protocol::ClientHello &hello,
		    time_point now);
		void handle_authenticate(
		    connection_id connection,
		    const protocol::ClientAuthenticate &message,
		    time_point now);
		void handle_world_ready(
		    connection_id connection,
		    const protocol::ClientWorldReady &message,
		    time_point now);
		void handle_world_failed(
		    connection_id connection,
		    const protocol::ClientWorldFailed &message,
		    time_point now);
		void handle_profile_update(
		    player_session &player,
		    const protocol::ClientProfileUpdate &message,
		    time_point now);
		void handle_world_object_update(
		    player_session &player,
		    const protocol::ClientWorldObjectUpdate &message);
		void handle_world_item_update(
		    player_session &player,
		    const protocol::ClientWorldItemUpdate &message);
		void handle_npc_discovery(
		    player_session &player,
		    const protocol::ClientNpcDiscovery &message,
		    time_point now);
		void handle_npc_update(
		    player_session &player,
		    const protocol::ClientNpcUpdate &message,
		    time_point now);
		void handle_transform(
		    player_session &player,
		    const protocol::ClientTransform &message,
		    time_point now);
		void handle_avatar_update(
		    player_session &player,
		    const protocol::ClientAvatarUpdate &message,
		    time_point now);
		void handle_chat(
		    player_session &player,
		    const protocol::ChatSend &message,
		    time_point now);
		void handle_voice(
		    player_session &player,
		    const protocol::ClientVoiceFrame &message,
		    time_point now);
		[[nodiscard]] bool handle_admin_chat(
		    player_session &player,
		    std::string_view text,
		    time_point now);
		void send_chat_message(
		    connection_id connection,
		    player_id sender,
		    std::string_view display_name,
		    std::string text,
		    protocol::ChatChannel channel,
		    time_point now);
		void send_system_message(
		    player_session &player,
		    std::string text,
		    time_point now,
		    protocol::ChatChannel channel = protocol::CHAT_CHANNEL_SYSTEM);
		void send_spatial_chat(
		    const player_session &sender,
		    std::string text,
		    protocol::ChatChannel channel,
		    float range_m,
		    time_point now);
		[[nodiscard]] bool teleport_player(
		    player_session &target,
		    const protocol::TransformState &destination,
		    std::string reason,
		    time_point now);
		void handle_ping(
		    player_session &player,
		    const protocol::Ping &message,
		    time_point now);
		void handle_sleep_state(
		    player_session &player,
		    const protocol::ClientSleepState &message,
		    time_point now);
		void handle_death(player_session &player, time_point now);
		void handle_respawn_request(player_session &player, time_point now);
		void handle_activity_start(
		    player_session &player,
		    const protocol::ClientActivityStart &message);
		void handle_activity_end(
		    player_session &player,
		    const protocol::ClientActivityEnd &message);
		void release_activity(
		    player_session &player,
		    const protocol::TransformState *final_transform = nullptr);
		void reject(
		    connection_id connection,
		    protocol::RejectReason reason,
		    std::string message,
		    const authentication_failure *failure = nullptr);
		void remove_player(
		    player_id id,
		    std::string reason,
		    close_kind close,
		    time_point now);
		void send_accepted(player_session &player);
		void send_entity_control(connection_id connection);
		void send_world_objects(connection_id connection);
		void send_world_items(connection_id connection);
		void broadcast_home_markers();
		void advance_environment_clock(time_point now);
		void broadcast_environment(time_point now);
		void broadcast_sleep_state(bool time_skipped = false);
		void broadcast_system_message(
		    std::string text,
		    time_point now,
		    std::optional<connection_id> except = std::nullopt);
		void remove_sleep_vote(player_id id);
		[[nodiscard]] std::uint32_t effective_sleep_requirement() const;
		void apply_default_avatar(protocol::PlayerProfile &profile);
		[[nodiscard]] bool avatar_allowed(
		    const protocol::AvatarDescriptor &avatar) const;
		[[nodiscard]] protocol::AvatarPolicy avatar_policy() const;
		void send_challenge(
		    connection_id connection,
		    std::uint64_t client_features);
		void send_bootstrap(connection_id connection, protocol::BootstrapMode mode);
		void release_initializer(connection_id connection);
		void wake_bootstrap_waiters();
		void persist_player(player_session &player, time_point now);
		void persist_world_objects();
		void persist_world_items();
		void remove_owned_items_from_world();
		void rebuild_avatar_equipment(player_session &player);
		void tick_dummies(time_point now);
		void begin_dummy_input(player_session &player, time_point now);
		[[nodiscard]] static std::uint64_t next_dummy_random(
		    player_session &player);
		void broadcast(
		    protocol::Envelope envelope,
		    reliability delivery,
		    std::optional<connection_id> except = std::nullopt);
		void queue(
		    connection_id connection,
		    protocol::Envelope envelope,
		    reliability delivery,
		    close_kind close = close_kind::none);
		void queue_snapshot(time_point now);
		[[nodiscard]] std::vector<npc_registry::player_position>
		player_positions() const;
		void queue_npc_events(std::vector<npc_registry::event> events);
		void queue_npc_snapshots();
		[[nodiscard]] player_session *find_by_connection(connection_id connection);
		[[nodiscard]] player_session *find_by_resume_token(std::string_view token);
		[[nodiscard]] static std::string lower_ascii(std::string_view value);
		[[nodiscard]] static std::uint64_t milliseconds(time_point value);
		[[nodiscard]] static protocol::PlayerSnapshot snapshot_of(
		    const player_session &player,
		    bool include_avatar);

		server_config m_config;
		token_generator m_generate_token;
		account_authenticator m_authenticate_account;
		moderation_executor m_moderate_account;
		world_store m_store;
		permission_store m_permissions;
		npc_registry m_npcs;
		property::service m_properties;
		std::uint64_t m_server_tick{};
		time_point m_last_snapshot{};
		std::unordered_map<connection_id, pending_connection> m_pending;
		std::unordered_map<player_id, player_session> m_players;
		std::unordered_map<player_id, profile_claim> m_claims;
		std::unordered_map<std::uint64_t, protocol::WorldObjectState>
		    m_world_objects;
		std::unordered_map<std::string, protocol::WorldItemState> m_world_items;
		item_ledger m_items;
		struct npc_delivery_state
		{
			std::uint64_t motion_revision{};
			std::uint64_t gameplay_revision{};
			std::uint64_t inventory_revision{};
			time_point motion_sent_at{};
		};
		std::unordered_map<player_id,
		    std::unordered_map<std::uint64_t, npc_delivery_state>>
		    m_npc_delivery;
		std::optional<connection_id> m_initializer;
		std::uint64_t m_next_dummy_index{1};
		bool m_human_npcs_disabled{};
		bool m_animal_npcs_disabled{};
		std::uint64_t m_environment_revision{1};
		std::uint64_t m_weather_revision{1};
		double m_environment_anchor_world_seconds{};
		float m_environment_time_scale{};
		std::uint32_t m_environment_weather_id{};
		std::uint32_t m_environment_weather_transition_ms{};
		time_point m_environment_anchor_time{};
		time_point m_current_time{};
		bool m_environment_clock_started{};
		std::unordered_set<player_id> m_sleeping_players;
		std::uint64_t m_sleep_revision{1};
		std::unordered_map<std::uint64_t, player_id> m_station_owners;
		std::uint64_t m_next_activity_session_id{1};
		std::vector<outbound_message> m_outbound;
	};
}
