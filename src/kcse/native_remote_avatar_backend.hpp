#pragma once

#include "kcse/native_entity_backend.hpp"
#include "kcse/native_remote_avatar_equipment.hpp"
#include "kcse/remote_avatar_readiness.hpp"
#include "multiplayer/remote_avatar.hpp"
#include "multiplayer/remote_locomotion_animation.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kcd2o::kcse
{
	class native_remote_avatar_backend final : public remote_avatar_backend
	{
	public:
		enum class active_probe_result
		{
			pending,
			succeeded,
			failed
		};

		explicit native_remote_avatar_backend(native_entity_backend &entities);

		void advance_frame() noexcept;
		void set_epoch(std::uint64_t epoch);
		void clear();
		void reset_active_probe();
		[[nodiscard]] std::uint32_t entity_id_for(player_id player) const noexcept;
		[[nodiscard]] active_probe_result poll_active_probe(
		    const protocol::TransformState &origin,
		    std::string &error);

		[[nodiscard]] bool available() const override;
		[[nodiscard]] std::string diagnostic() const override;
		[[nodiscard]] std::optional<remote_avatar_handle> spawn(
		    const remote_avatar_snapshot &player) override;
		[[nodiscard]] remote_avatar_backend_status status(
		    remote_avatar_handle avatar) const override;
		[[nodiscard]] bool update(
		    remote_avatar_handle avatar,
		    const remote_avatar_snapshot &player,
		    bool appearance_changed) override;
		void remove(remote_avatar_handle avatar) override;

	private:
		enum class multiplayer_semantics_result
		{
			applied,
			deferred,
			failed
		};

		struct entry
		{
			player_id player{};
			std::uint32_t entity_id{};
			std::string entity_name;
			std::chrono::steady_clock::time_point spawn_requested_at{};
			std::optional<native_entity_backend::human_npc_spawn_scope>
			    spawn_authorization;
			std::uint64_t epoch{};
			std::uint64_t shared_soul_applied_frame{};
			std::chrono::steady_clock::time_point
			    shared_soul_applied_at{};
			bool lifecycle_ready{};
			bool presented{};
			bool display_name_applied{};
			std::string display_name;
			bool multiplayer_semantics_applied{};
			std::chrono::steady_clock::time_point
			    next_multiplayer_semantics_attempt{};
			bool appearance_applied{};
			bool transform_applied{};
			protocol::TransformState last_transform;
			bool motion_applied{};
			protocol::MovementMode last_movement_mode{
			    protocol::MOVEMENT_MODE_IDLE};
			remote_locomotion_animation locomotion_animation{
			    remote_locomotion_animation::idle};
			bool sprint_animation_supported{true};
			float smoothed_visual_speed{};
			float last_visual_x{};
			float last_visual_y{};
			bool visual_position_sampled{};
			std::chrono::steady_clock::time_point
			    last_visual_sample_at{};
			std::chrono::steady_clock::time_point
			    last_motion_request_at{};
			std::chrono::steady_clock::time_point
			    next_motion_retry_at{};
			std::chrono::steady_clock::time_point
			    last_native_transform_at{};
			std::uint64_t last_animation_sequence{};
			bool one_shot_animation_active{};
			std::string one_shot_animation_clip;
			std::string presented_animation_clip;
			bool presented_animation_loop{};
			std::chrono::steady_clock::time_point
			    last_native_validation_at{};
			bool first_transform_logged{};
			bool first_motion_logged{};
			bool first_weapon_action_logged{};
			bool native_weapon_actions_enabled{true};
			bool failed{};
			std::string failure;
			protocol::AvatarDescriptor appearance;
			std::vector<native_remote_equipment_instance> item_instances;
			bool activity_active{};
			protocol::PlayerActivityKind activity_kind{
			    protocol::PLAYER_ACTIVITY_KIND_NONE};
			std::uint64_t activity_session_id{};
			std::uint64_t activity_station_guid{};
		};

		[[nodiscard]] entry *find(remote_avatar_handle avatar);
		[[nodiscard]] const entry *find(remote_avatar_handle avatar) const;
		[[nodiscard]] remote_avatar_backend_status status_impl(
		    remote_avatar_handle avatar) const;
		[[nodiscard]] bool apply_appearance(
		    entry &avatar,
		    const protocol::AvatarDescriptor &appearance,
		    std::string &error);
		[[nodiscard]] bool remove_created_items(
		    entry &avatar,
		    std::string &error);
		[[nodiscard]] bool update_motion_state(
		    entry &avatar,
		    const remote_avatar_snapshot &player,
		    std::string &error);
		void update_animation_state(
		    entry &avatar,
		    const remote_avatar_snapshot &player);
		[[nodiscard]] bool present_animation(
		    entry &avatar,
		    const remote_avatar_snapshot &player,
		    std::string &error);
		[[nodiscard]] bool apply_display_name(
		    entry &avatar,
		    const remote_avatar_snapshot &player,
		    std::string &error);
		[[nodiscard]] bool present(
		    entry &avatar,
		    std::string &error);
		[[nodiscard]] multiplayer_semantics_result apply_multiplayer_semantics(
		    entry &avatar,
		    std::string &error);
		[[nodiscard]] bool apply_activity(
		    entry &avatar,
		    const remote_avatar_snapshot &player,
		    std::string &error);

		native_entity_backend &m_entities;
		mutable std::unordered_map<remote_avatar_handle, entry> m_avatars;
		std::optional<remote_avatar_handle> m_probe_avatar;
		remote_avatar_snapshot m_probe_snapshot;
		std::uint32_t m_probe_polls{};
		std::uint64_t m_frame_sequence{};
		std::uint64_t m_epoch{1};
		remote_avatar_handle m_next_handle{1};
		mutable bool m_catalogs_ready{};
		mutable std::string m_diagnostic;
	};
}
