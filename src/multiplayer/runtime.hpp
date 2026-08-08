#pragma once

#include "kcd2o.pb.h"
#include "multiplayer/runtime_capabilities.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kcd2o
{
	enum class sandbox_phase
	{
		idle,
		loading,
		ready,
		failed,
		unloading
	};

	struct runtime_descriptor
	{
		std::uint64_t capabilities{};
		std::uint32_t kcse_version{};
		std::uint32_t game_version{};
		std::uint32_t release_index{};
		std::uint64_t epoch{};
		std::string address_library;
		std::string address_library_distribution;
		std::uint32_t address_library_format{};
		std::uint32_t address_library_entries{};
		std::string address_library_sha256;
	};

	struct runtime_gate
	{
		bool available{};
		bool pending{};
		std::string diagnostic;
	};

	struct sandbox_start_result
	{
		bool started{};
		std::string error;
	};

	struct sandbox_poll_result
	{
		sandbox_phase phase{sandbox_phase::idle};
		std::string error;
		std::optional<protocol::TransformState> initial_spawn;
	};

	enum class sandbox_spawn_source
	{
		none,
		profile,
		server,
		local_engine_default
	};

	struct sandbox_spawn_selection
	{
		std::optional<protocol::TransformState> transform;
		sandbox_spawn_source source{sandbox_spawn_source::none};
	};

	[[nodiscard]] inline sandbox_spawn_selection select_sandbox_spawn(
	    const protocol::ServerBootstrap &bootstrap,
	    const std::optional<protocol::TransformState> &local_transform)
	{
		if (bootstrap.has_profile()
		    && bootstrap.profile().transform_valid()
		    && bootstrap.profile().has_last_transform())
		{
			return {
			    bootstrap.profile().last_transform(),
			    sandbox_spawn_source::profile};
		}
		if (bootstrap.spawn_valid() && bootstrap.has_spawn())
		{
			return {
			    bootstrap.spawn(),
			    sandbox_spawn_source::server};
		}
		if (bootstrap.mode() == protocol::BOOTSTRAP_MODE_INITIALIZE
		    && local_transform)
		{
			return {
			    *local_transform,
			    sandbox_spawn_source::local_engine_default};
		}
		return {};
	}

	[[nodiscard]] inline const char *to_string(
	    sandbox_spawn_source source) noexcept
	{
		switch (source)
		{
		case sandbox_spawn_source::profile:
			return "profile.last_transform";
		case sandbox_spawn_source::server:
			return "bootstrap.spawn";
		case sandbox_spawn_source::local_engine_default:
			return "local.engine-default";
		default:
			return "none";
		}
	}

	class client_runtime
	{
	public:
		virtual ~client_runtime() = default;

		[[nodiscard]] virtual runtime_descriptor descriptor() const = 0;
		[[nodiscard]] virtual runtime_gate capability() const = 0;
		[[nodiscard]] virtual bool can_start_join() const = 0;
		[[nodiscard]] virtual bool prepare_multiplayer() = 0;
		virtual void cancel_multiplayer_preparation() = 0;
		[[nodiscard]] virtual sandbox_start_result begin_sandbox(
		    const protocol::ServerBootstrap &bootstrap) = 0;
		[[nodiscard]] virtual sandbox_poll_result poll_sandbox() = 0;
		[[nodiscard]] virtual bool sandbox_active() const = 0;
		virtual void end_sandbox(std::string_view error = {}) = 0;
		[[nodiscard]] virtual std::string current_level_id() const = 0;
		[[nodiscard]] virtual std::optional<protocol::PlayerProfile>
		local_profile() = 0;
		[[nodiscard]] virtual bool set_npc_entities_disabled(
		    bool humans_disabled,
		    bool animals_disabled) = 0;
		[[nodiscard]] virtual std::vector<protocol::WorldObjectState>
		poll_world_object_updates()
		{
			return {};
		}
		[[nodiscard]] virtual bool apply_world_object_state(
		    const protocol::WorldObjectState &)
		{
			return true;
		}
		[[nodiscard]] virtual std::vector<protocol::WorldItemState>
		poll_world_item_updates()
		{
			return {};
		}
		[[nodiscard]] virtual bool apply_world_item_state(
		    const protocol::WorldItemState &)
		{
			return true;
		}
		[[nodiscard]] virtual std::vector<protocol::NpcObservation>
		poll_npc_observations()
		{
			return {};
		}
		[[nodiscard]] virtual bool apply_npc_state(
		    const protocol::NpcState &,
		    bool)
		{
			return true;
		}
		virtual void remove_npc_state(std::uint64_t, std::uint32_t)
		{
		}
		[[nodiscard]] virtual bool apply_environment_state(
		    const protocol::EnvironmentState &,
		    bool)
		{
			return true;
		}
		[[nodiscard]] virtual bool set_home_marker(
		    const std::optional<protocol::PropertyHomeMarker> &)
		{
			return true;
		}
		[[nodiscard]] virtual bool apply_authoritative_profile(
		    const protocol::PlayerProfile &)
		{
			return false;
		}
		[[nodiscard]] virtual bool respawn_local_player(
		    const protocol::TransformState &)
		{
			return false;
		}
		virtual void show_multiplayer_notice(std::string_view)
		{
		}
	};
}
