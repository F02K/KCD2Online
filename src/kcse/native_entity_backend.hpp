#pragma once

#include "kcse/native_world_object_sync.hpp"
#include "kcse/native_world_item_sync.hpp"
#include "kcd2o.pb.h"

#include <Offsets/vtables/IEntitySystem.h>

#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Offsets
{
	struct IEntity;
}

namespace wh::entitymodule
{
	class C_Actor;
}

namespace kcd2o::kcse
{
	struct native_player_view
	{
		Offsets::IEntity *entity{};
		wh::entitymodule::C_Actor *actor{};
	};

	class native_entity_backend
	{
	public:
		class human_npc_spawn_scope
		{
		public:
			human_npc_spawn_scope(const human_npc_spawn_scope &) = delete;
			human_npc_spawn_scope &operator=(
			    const human_npc_spawn_scope &) = delete;
			human_npc_spawn_scope(human_npc_spawn_scope &&other) noexcept;
			human_npc_spawn_scope &operator=(
			    human_npc_spawn_scope &&other) noexcept;
			~human_npc_spawn_scope();

		private:
			friend class native_entity_backend;
			human_npc_spawn_scope(
			    native_entity_backend &owner,
			    std::uint64_t token) noexcept;

			native_entity_backend *m_owner{};
			std::uint64_t m_token{};
		};

		native_entity_backend();
		~native_entity_backend();

		[[nodiscard]] native_player_view player() const;
		[[nodiscard]] std::optional<protocol::TransformState> read_transform(
		    Offsets::IEntity *entity) const;
		[[nodiscard]] bool write_transform(
		    Offsets::IEntity *entity,
		    const protocol::TransformState &transform,
		    std::string &error) const;
		[[nodiscard]] bool set_world_isolated(
		    bool humans_disabled,
		    bool animals_disabled,
		    std::string &error);
		void register_player_entity(
		    std::uint32_t entity_id,
		    std::uint64_t player_id = 0);
		void unregister_player_entity(std::uint32_t entity_id);
		[[nodiscard]] human_npc_spawn_scope authorize_human_npc_spawn(
		    std::string entity_name);
		void process_pending_entity_control();
		[[nodiscard]] bool begin_world_sync(std::string &error);
		void restore_world();
		[[nodiscard]] std::vector<protocol::WorldObjectState>
		poll_world_object_updates();
		[[nodiscard]] bool apply_world_object_state(
		    const protocol::WorldObjectState &state,
		    std::string &error);
		[[nodiscard]] std::vector<protocol::WorldItemState>
		poll_world_item_updates();
		[[nodiscard]] bool apply_world_item_state(
		    const protocol::WorldItemState &state,
		    std::string &error);
		[[nodiscard]] std::vector<protocol::NpcObservation>
		poll_npc_observations();
		[[nodiscard]] bool apply_npc_state(
		    const protocol::NpcState &state,
		    bool local_authority,
		    std::string &error);
		void remove_npc_state(std::uint64_t npc_id, std::uint32_t generation);
		void reset_world_sync();

	private:
		class isolation_sink final : public Offsets::IEntitySystemSink
		{
		public:
			void attach(native_entity_backend &owner);
			bool OnBeforeSpawn(void *params) override;
			void OnSpawn(Offsets::IEntity *entity, void *params) override;
			bool OnRemove(Offsets::IEntity *entity) override;
			void OnReused(Offsets::IEntity *entity, void *params) override;
			void _vf5(Offsets::IEntity *entity, void *event) override;
			void OnEvent(Offsets::IEntity *entity, void *event) override;
			void GetMemoryUsage(void *sizer) const override;

		private:
			native_entity_backend *m_owner{};
		};

		// CryEngine declares OnAfterInit before the virtual destructor, so this
		// order is ABI-significant for the two-slot IGameObjectSystemSink vtable.
		class game_object_init_sink final
		{
		public:
			virtual void OnAfterInit(void *game_object);
			virtual ~game_object_init_sink() = default;
			void attach(native_entity_backend &owner);

		private:
			native_entity_backend *m_owner{};
		};

		struct pending_entity
		{
			std::uint16_t waited_frames{};
			bool game_object_initialized{};
		};
		struct entity_state
		{
			bool hidden{};
			bool active{};
		};
		struct managed_npc
		{
			std::uint64_t authored_guid{};
			std::uint32_t entity_id{};
			std::uint32_t generation{};
			std::uint64_t gameplay_revision{};
			std::uint64_t inventory_revision{};
			std::uint64_t dialog_revision{};
			std::chrono::steady_clock::time_point next_inventory_sample{};
			entity_state original;
			bool in_interest{};
			bool local_authority{};
		};
		struct cached_npc
		{
			std::uint64_t guid{};
			protocol::NpcKind kind{protocol::NPC_KIND_UNSPECIFIED};
		};
		struct human_npc_spawn_authorization
		{
			std::uint64_t token{};
			std::string entity_name;
			std::thread::id thread;
			bool consumed{};
		};
		void ensure_sink_registered(Offsets::IEntitySystem &system);
		void ensure_game_object_sink_registered(void *system);
		void queue_entity_for_control(
		    Offsets::IEntity *entity,
		    bool game_object_initialized,
		    bool actor_class_confirmed);
		void game_object_initialized(std::uint32_t entity_id);
		void refresh_local_player_exclusion(Offsets::IEntitySystem &system);
		void refresh_actor_roster(Offsets::IEntitySystem &system);
		void maintain_isolated_entities(Offsets::IEntitySystem &system);
		void maintain_managed_npcs(Offsets::IEntitySystem &system);
		[[nodiscard]] bool isolate_npc_entity(Offsets::IEntity *entity);
		[[nodiscard]] bool should_isolate_npc_actor(
		    Offsets::IEntity *entity);
		[[nodiscard]] std::optional<protocol::NpcKind> classify_npc_actor(
		    Offsets::IEntity *entity);
		void entity_removed(Offsets::IEntity *entity);
		void entity_event(Offsets::IEntity *entity, void *event);
		[[nodiscard]] bool allow_human_npc_spawn(void *params);
		[[nodiscard]] bool managed_human_spawn_active() const;
		void end_human_npc_spawn_authorization(std::uint64_t token);

		isolation_sink m_sink;
		game_object_init_sink m_game_object_sink;
		native_world_object_sync m_world_sync;
		native_world_item_sync m_world_item_sync;
		Offsets::IEntitySystem *m_sink_system{};
		void *m_game_object_system{};
		std::unordered_map<std::uint32_t, entity_state> m_isolated;
		std::unordered_map<std::uint64_t, managed_npc> m_managed_npcs;
		std::unordered_map<std::uint32_t, std::uint64_t> m_managed_npc_by_entity;
		std::unordered_map<std::uint32_t, cached_npc> m_npc_roster;
		std::unordered_set<std::uint32_t> m_player_entities;
		std::unordered_map<std::uint64_t, std::uint32_t> m_player_entity_ids;
		std::unordered_map<std::uint32_t, pending_entity> m_pending_control;
		std::unordered_set<void *> m_human_npc_classes;
		std::vector<human_npc_spawn_authorization>
		    m_human_npc_spawn_authorizations;
		std::uint32_t m_local_player_entity_id{};
		std::uint32_t m_isolation_maintenance_frame{};
		std::uint64_t m_next_human_npc_spawn_token{};
		std::chrono::steady_clock::time_point m_last_npc_roster_refresh{};
		int m_last_actor_count{-1};
		bool m_human_npcs_disabled{};
		bool m_animal_npcs_disabled{};
		bool m_isolation_active{};
	};
}
