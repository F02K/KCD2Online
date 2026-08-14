#pragma once

#include "kcse/native_entity_backend.hpp"
#include "kcse/native_profile_backend.hpp"
#include "kcse/native_remote_avatar_backend.hpp"
#include "kcse/native_voice.hpp"
#include "multiplayer/runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <KCSE/KCSEAPI.h>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wh::playermodule
{
	class I_Minigame;
}

namespace wh::guimodule
{
	class C_UIMap;
	struct S_EntityMapMark;
} // namespace wh::guimodule

namespace kcd2o::kcse
{
	struct local_activity_start
	{
		protocol::PlayerActivityKind kind{protocol::PLAYER_ACTIVITY_KIND_NONE};
		std::uint64_t station_guid{};
	};

	class native_runtime final : public client_runtime
	{
	public:
		explicit native_runtime(const KCSE::IKCSEInterface &kcse);

		void on_lifecycle(std::uint32_t message_type) noexcept;
		[[nodiscard]] bool on_frame();
		void on_blacksmithing_started(std::uint32_t station_entity_id);
		[[nodiscard]] std::optional<local_activity_start> take_local_activity_start();
		[[nodiscard]] bool local_activity_end_pending() const noexcept;
		void acknowledge_local_activity_end() noexcept;
		void cancel_local_activity();

		[[nodiscard]] runtime_descriptor descriptor() const override;
		[[nodiscard]] runtime_gate capability() const override;
		[[nodiscard]] bool can_start_join() const override;
		[[nodiscard]] bool prepare_multiplayer() override;
		void cancel_multiplayer_preparation() override;
		[[nodiscard]] sandbox_start_result begin_sandbox(const protocol::ServerBootstrap &bootstrap) override;
		[[nodiscard]] sandbox_poll_result poll_sandbox() override;
		[[nodiscard]] bool sandbox_active() const override;
		void end_sandbox(std::string_view error = {}) override;
		[[nodiscard]] std::string current_level_id() const override;
		[[nodiscard]] std::optional<protocol::PlayerProfile> local_profile() override;
		[[nodiscard]] bool set_npc_entities_disabled(bool humans_disabled, bool animals_disabled) override;
		[[nodiscard]] std::vector<protocol::WorldObjectState> poll_world_object_updates() override;
		[[nodiscard]] bool apply_world_object_state(const protocol::WorldObjectState &state) override;
		[[nodiscard]] std::vector<protocol::WorldItemState> poll_world_item_updates() override;
		[[nodiscard]] bool apply_world_item_state(const protocol::WorldItemState &state) override;
		[[nodiscard]] std::vector<protocol::NpcObservation> poll_npc_observations() override;
		[[nodiscard]] bool apply_npc_state(const protocol::NpcState &state, bool local_authority) override;
		void remove_npc_state(std::uint64_t npc_id, std::uint32_t generation) override;
		[[nodiscard]] bool apply_environment_state(const protocol::EnvironmentState &state, bool apply_weather) override;
		[[nodiscard]] bool set_home_marker(const std::optional<protocol::PropertyHomeMarker> &marker) override;
		[[nodiscard]] bool apply_authoritative_profile(const protocol::PlayerProfile &profile) override;
		[[nodiscard]] bool respawn_local_player(const protocol::TransformState &spawn) override;
		[[nodiscard]] bool local_player_dead() const;
		[[nodiscard]] bool local_player_laying() const;
		[[nodiscard]] bool play_emote(std::string_view fragment);
		void show_multiplayer_notice(std::string_view message) override;
		void set_voice_active(bool active) override;
		[[nodiscard]] voice_capture_state voice_status() const noexcept;
		[[nodiscard]] std::vector<protocol::ClientVoiceFrame> poll_outbound_voice() override;
		void receive_voice(const protocol::ServerVoiceFrame &frame) override;
		[[nodiscard]] bool set_player_voice_volume(player_id player, float volume) noexcept;
		void reset_voice() override;

		[[nodiscard]] std::optional<protocol::TransformState> local_transform() const;
		[[nodiscard]] std::optional<protocol::AvatarDescriptor> local_avatar_visual() const;
		[[nodiscard]] bool apply_local_correction(const protocol::TransformState &transform);
		[[nodiscard]] remote_avatar_sync_result sync_remote_players(std::span<const remote_avatar_snapshot> players);
		[[nodiscard]] std::uint64_t epoch() const noexcept;

	private:
		enum class world_start_stage
		{
			idle,
			invoking_new_game,
			waiting_for_lifecycle,
			waiting_for_data,
			waiting_for_level,
			waiting_for_player,
			probing_runtime,
			activating,
			failed
		};

		void invalidate_epoch_on_game_thread();
		void refresh_cached_state();
		[[nodiscard]] sandbox_start_result activate_loaded_sandbox(const protocol::ServerBootstrap &bootstrap);
		[[nodiscard]] sandbox_start_result begin_native_world_start(const protocol::ServerBootstrap &bootstrap);
		void advance_native_world_start();
		void set_world_start_stage(world_start_stage stage, std::string diagnostic);
		void fail_native_world_start(std::string error);
		void restore_save_load();
		void begin_native_unload(std::string_view reason);
		void queue_native_unload_if_safe();
		void finish_native_unload_if_complete();
		[[nodiscard]] bool native_world_unloaded() const;
		void poll_local_activity();
		void refresh_home_marker();
		void remove_home_marker();
		[[nodiscard]] wh::playermodule::I_Minigame *find_local_minigame(protocol::PlayerActivityKind kind) const;

		const KCSE::IKCSEInterface &m_kcse;
		std::string m_address_library;
		std::string m_address_library_distribution;
		std::uint32_t m_address_library_format{};
		std::uint32_t m_address_library_entries{};
		std::string m_address_library_sha256;
		std::atomic<std::uint64_t> m_epoch{1};
		std::atomic<bool> m_epoch_invalidated{};
		std::atomic<bool> m_data_loaded{};
		std::atomic<bool> m_frame_seen{};
		std::atomic<bool> m_multiplayer_requested{};
		std::atomic<bool> m_expected_epoch_transition{};
		std::atomic<bool> m_world_lifecycle_seen{};
		std::atomic<bool> m_world_pre_data_seen{};

		mutable std::mutex m_cache_mutex;
		std::uint64_t m_capabilities{};
		std::string m_level_id;
		std::optional<protocol::TransformState> m_local_transform;
		std::chrono::steady_clock::time_point m_local_transform_sampled_at{};
		std::uint64_t m_transform_sequence{};
		std::string m_local_animation_fragment;
		std::uint64_t m_animation_sequence{};
		std::uint64_t m_animation_started_at_ms{};
		std::string m_explicit_animation_fragment;
		std::chrono::steady_clock::time_point m_explicit_animation_ends_at{};
		std::string m_diagnostic;
		bool m_transition_safe{};
		std::string m_transition_blocker;
		bool m_sandbox_active{};
		sandbox_poll_result m_sandbox_progress;
		bool m_save_load_locked{};
		bool m_unload_pending{};
		bool m_unload_teardown_started{};
		std::uint64_t m_unload_teardown_frame{};
		bool m_unload_command_queued{};
		bool m_unload_deferred_logged{};
		bool m_level_load_complete{};
		std::uint64_t m_frame_sequence{};
		bool m_probe_transform_verified{};
		bool m_probe_complete{};
		std::uint64_t m_native_weather_revision{};
		std::atomic<bool> m_probe_failed{};
		bool m_preparation_active{};
		std::uint32_t m_preparation_frames{};
		std::string m_probe_error;
		world_start_stage m_world_start_stage{world_start_stage::idle};
		std::optional<protocol::ServerBootstrap> m_world_start_bootstrap;
		std::string m_world_start_level_id;
		std::string m_world_start_level_name;
		bool m_world_start_requires_lifecycle{};
		std::optional<local_activity_start> m_pending_activity_start;
		protocol::PlayerActivityKind m_native_activity_kind{protocol::PLAYER_ACTIVITY_KIND_NONE};
		bool m_activity_end_pending{};
		std::optional<protocol::PropertyHomeMarker> m_home_marker;
		std::chrono::steady_clock::time_point m_next_home_marker_attempt{};
		std::shared_ptr<wh::guimodule::S_EntityMapMark> m_native_home_mark;
		wh::guimodule::C_UIMap *m_native_home_map{};
		bool m_home_filter_was_visible{};
		native_entity_backend m_entities;
		native_profile_backend m_profiles;
		native_remote_avatar_backend m_remote_backend;
		remote_avatar_manager m_remote_avatars;
		native_voice m_voice;
	};
} // namespace kcd2o::kcse
