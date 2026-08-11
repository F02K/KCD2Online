#pragma once

#include "multiplayer/game_command_queue.hpp"
#include "multiplayer/identity_store.hpp"
#include "multiplayer/networking.hpp"
#include "multiplayer/remote_transform_sequence.hpp"
#include "multiplayer/runtime.hpp"
#include "multiplayer/client_state.hpp"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace kcd2o
{
	struct client_options
	{
		std::string address{"127.0.0.1:27020"};
		std::string display_name{"Henry"};
		std::string password;
		std::string content_hash;
		std::string claim_code;
		std::string server_id;
		std::string account_service_url{"https://api.kingdom-online.cc"};
	};

	struct chat_entry
	{
		player_id sender{};
		std::string display_name;
		std::string text;
		std::uint64_t server_time_ms{};
		protocol::ChatChannel channel{protocol::CHAT_CHANNEL_SAY};
		protocol::NetworkRole network_role{protocol::NETWORK_ROLE_USER};
	};

	struct remote_player_view
	{
		player_id id{};
		std::string display_name;
		bool connected{};
		bool has_transform{};
		protocol::TransformState transform;
		protocol::MovementMode movement_mode{protocol::MOVEMENT_MODE_IDLE};
		protocol::AvatarDescriptor avatar;
		bool has_avatar{};
		protocol::PlayerActivity activity;
		bool has_activity{};
		protocol::NetworkRole network_role{protocol::NETWORK_ROLE_USER};
	};

	struct client_status
	{
		client_state state{client_state::disconnected};
		player_id local_player_id{};
		std::string server_name;
		std::string server_id;
		std::string session_id;
		std::string level_id;
		std::string error;
		int ping_ms{-1};
		float packet_loss_percent{};
		std::size_t game_queue_size{};
		protocol::AvatarPolicy avatar_policy;
		std::string avatar_archetype_id;
		bool sleeping{};
		std::uint32_t sleeping_players{};
		std::uint32_t sleeping_players_required{1};
		bool dead{};
		bool respawn_pending{};
		bool voice_recording{};
		bool voice_speaking{};
		float voice_level{};
		protocol::VoiceRange voice_range{protocol::VOICE_RANGE_NORMAL};
		bool native_keybinds{};
		std::uint32_t chat_action_generation{};
		bool emote_action_held{};
		protocol::NetworkRole network_role{protocol::NETWORK_ROLE_USER};
	};

	struct client_update_rates
	{
		std::uint32_t tick_rate{30};
		std::uint32_t snapshot_rate{20};
	};

	class multiplayer_client
	{
	public:
		explicit multiplayer_client(client_runtime &runtime);
		~multiplayer_client();
		multiplayer_client(const multiplayer_client &) = delete;
		multiplayer_client &operator=(const multiplayer_client &) = delete;

		[[nodiscard]] bool connect(client_options options);
		void disconnect();
		void fail(std::string error);
		[[nodiscard]] bool send_chat(std::string text);
		[[nodiscard]] bool select_avatar(std::string archetype_id);
		[[nodiscard]] bool set_sleeping(bool sleeping);
		void report_local_death();
		[[nodiscard]] bool request_respawn();
		[[nodiscard]] bool begin_local_activity(
		    protocol::PlayerActivityKind kind,
		    std::uint64_t station_guid);
		[[nodiscard]] bool end_local_activity(
		    std::optional<protocol::TransformState> final_transform = std::nullopt);
		[[nodiscard]] std::optional<std::string> take_activity_denial();
		void runtime_epoch_changed();
		[[nodiscard]] bool reserve_local_avatar_sample(
		    std::chrono::steady_clock::time_point now =
		        std::chrono::steady_clock::now());
		void game_tick(
		    std::optional<protocol::TransformState> local_transform,
		    std::optional<protocol::AvatarDescriptor> local_avatar_visual,
		    std::string_view current_level,
		    std::chrono::steady_clock::time_point now =
		        std::chrono::steady_clock::now());

		[[nodiscard]] client_status status() const;
		[[nodiscard]] client_update_rates update_rates() const;
		[[nodiscard]] std::vector<remote_player_view> remote_players() const;
		[[nodiscard]] std::vector<chat_entry> chat_history() const;
		[[nodiscard]] std::optional<protocol::TransformState> take_local_correction();

	private:
		client_runtime &m_runtime;

		struct connect_command
		{
			client_options options;
		};
		struct disconnect_command
		{
		};
		struct transform_command
		{
			protocol::TransformState transform;
		};
		struct chat_command
		{
			std::string text;
		};
		struct world_ready_command
		{
			protocol::ClientWorldReady message;
		};
		struct world_failed_command
		{
			protocol::ClientWorldFailed message;
		};
		struct profile_command
		{
			protocol::ClientProfileUpdate message;
		};
		struct avatar_command
		{
			protocol::ClientAvatarUpdate message;
		};
		struct world_object_command
		{
			protocol::ClientWorldObjectUpdate message;
		};
		struct world_item_command
		{
			protocol::ClientWorldItemUpdate message;
		};
		struct npc_discovery_command
		{
			protocol::ClientNpcDiscovery message;
		};
		struct npc_update_batch_command
		{
			protocol::ClientNpcUpdateBatch message;
		};
		struct sleep_command
		{
			bool sleeping{};
		};
		struct death_command
		{
		};
		struct respawn_command
		{
		};
		struct activity_start_command
		{
			protocol::ClientActivityStart message;
		};
		struct activity_end_command
		{
			protocol::ClientActivityEnd message;
		};
		struct voice_command
		{
			protocol::ClientVoiceFrame message;
		};
		using network_command = std::variant<
		    connect_command,
		    disconnect_command,
		    transform_command,
		    chat_command,
		    world_ready_command,
		    world_failed_command,
		    profile_command,
		    avatar_command,
		    world_object_command,
		    world_item_command,
		    npc_discovery_command,
		    npc_update_batch_command,
		    sleep_command,
		    death_command,
		    respawn_command,
		    activity_start_command,
		    activity_end_command,
		    voice_command>;

		struct timed_transform
		{
			std::chrono::steady_clock::time_point received_at;
			protocol::TransformState transform;
			protocol::MovementMode mode{protocol::MOVEMENT_MODE_IDLE};
			bool connected{};
		};

		struct remote_player
		{
			std::string display_name;
			std::deque<timed_transform> history;
			remote_player_view rendered;
			remote_transform_sequence transform_sequence;
		};

		void network_loop(std::stop_token stop);
		void advance_runtime_preflight();
		void ensure_network_thread();
		bool set_state(client_state state, std::string error = {});
		[[nodiscard]] bool transition_state_locked(
		    client_state state,
		    std::string error = {});
		void queue_network(network_command command);
		void queue_profile_snapshot(
		    protocol::PlayerProfile profile,
		    bool allow_closing = false);
		void queue_world_object_updates(
		    std::vector<protocol::WorldObjectState> updates);
		void queue_world_item_updates(
		    std::vector<protocol::WorldItemState> updates);
		void queue_npc_observations(
		    std::vector<protocol::NpcObservation> observations,
		    std::chrono::steady_clock::time_point now);
		void handle_game_envelope(
		    const protocol::Envelope &envelope,
		    std::chrono::steady_clock::time_point now);
		void advance_sandbox_bootstrap();
		void update_interpolation(std::chrono::steady_clock::time_point now);
		void accept_snapshot_player(
		    const protocol::PlayerSnapshot &snapshot,
		    std::chrono::steady_clock::time_point now,
		    bool reset_transform_stream = false);
		[[nodiscard]] static protocol::TransformState interpolate(
		    const protocol::TransformState &from,
		    const protocol::TransformState &to,
		    float factor);
		[[nodiscard]] static protocol::TransformState extrapolate(
		    const protocol::TransformState &from,
		    float seconds);

		mutable std::mutex m_state_mutex;
		client_status m_status;
		client_update_rates m_update_rates;
		std::string m_resume_token;
		std::string m_server_id;
		identity_store m_identities;
		std::unordered_map<player_id, remote_player> m_remote_players;
		mutable std::mutex m_chat_mutex;
		std::deque<chat_entry> m_chat;
		std::atomic_bool m_chat_connected{};
		std::optional<protocol::TransformState> m_local_correction;
		std::optional<protocol::PlayerProfile> m_profile;
		std::optional<protocol::PlayerProfile> m_pending_profile;
		std::optional<protocol::AvatarDescriptor> m_local_avatar;
		std::optional<protocol::AvatarDescriptor> m_pending_avatar;
		std::optional<protocol::AvatarDescriptor> m_desired_avatar;
		std::optional<std::string> m_desired_archetype;
		std::optional<protocol::PlayerActivity> m_local_activity;
		std::optional<protocol::ClientActivityStart> m_pending_activity_start;
		std::optional<std::string> m_activity_denial;
		bool m_manual_disconnect_pending{};
		bool m_disconnect_capture_profile{};
		bool m_avatar_update_pending{};
		std::optional<protocol::ServerBootstrap> m_pending_bootstrap;
		std::optional<client_options> m_pending_connect;
		bool m_profile_update_pending{};
		std::unordered_map<std::uint64_t, protocol::WorldObjectState>
		    m_world_objects;
		std::unordered_map<std::uint64_t, protocol::WorldObjectState>
		    m_pending_world_objects;
		std::unordered_map<std::uint64_t, protocol::WorldObjectState>
		    m_deferred_world_objects;
		std::unordered_map<std::string, protocol::WorldItemState> m_world_items;
		std::unordered_map<std::string, protocol::WorldItemState>
		    m_pending_world_items;
		std::unordered_map<std::string, protocol::WorldItemState>
		    m_deferred_world_items;
		std::unordered_map<std::uint64_t, protocol::NpcState> m_npcs;
		std::unordered_map<std::uint64_t, std::uint64_t> m_npc_by_guid;
		// Motion and gameplay use independent delivery streams. Keep the latest
		// motion revision separately so a reliable gameplay update cannot make a
		// later-arriving unreliable transform look stale.
		std::unordered_map<std::uint64_t, std::uint64_t> m_npc_motion_revisions;
		bool m_human_npcs_disabled{};
		bool m_animal_npcs_disabled{};
		std::uint32_t m_profile_snapshot_interval_seconds{15};
		std::uint64_t m_environment_revision{};
		std::uint64_t m_weather_revision{};
		std::uint64_t m_sleep_revision{};
		std::chrono::steady_clock::time_point m_last_environment_applied{};
		std::chrono::steady_clock::time_point m_last_weather_applied{};

		mutable std::mutex m_network_mutex;
		std::deque<network_command> m_network_commands;
		game_command_queue m_game_commands;
		std::jthread m_network_thread;
		std::chrono::steady_clock::time_point m_last_transform_sent{};
		std::optional<protocol::TransformState> m_last_sent_transform;
		std::chrono::steady_clock::time_point m_last_profile_sent{};
		std::chrono::steady_clock::time_point m_last_avatar_sent{};
		std::chrono::steady_clock::time_point m_last_avatar_sampled{};
		std::chrono::steady_clock::time_point m_last_npc_sampled{};
		std::chrono::steady_clock::time_point m_last_npc_discovery_sent{};
	};
} // namespace kcd2o
