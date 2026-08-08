#pragma once

#include "kcd2o.pb.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Offsets
{
	struct IEntity;
}

namespace kcd2o::kcse
{
	enum class world_inventory_source : std::uint8_t
	{
		none,
		accessor,
		direct_stash
	};

	[[nodiscard]] constexpr protocol::WorldObjectKind classify_world_object(
	    world_inventory_source inventory_source,
	    bool is_door)
	{
		if (inventory_source != world_inventory_source::none)
			return protocol::WORLD_OBJECT_KIND_CONTAINER;
		if (is_door)
			return protocol::WORLD_OBJECT_KIND_DOOR;
		return protocol::WORLD_OBJECT_KIND_UNSPECIFIED;
	}

	[[nodiscard]] constexpr bool effective_world_object_opened(
	    protocol::WorldObjectKind kind,
	    bool script_opened,
	    bool tracked_container_open)
	{
		return script_opened
		    || (kind == protocol::WORLD_OBJECT_KIND_CONTAINER
		        && tracked_container_open);
	}

	class native_world_object_sync
	{
	public:
		void process();
		[[nodiscard]] std::vector<protocol::WorldObjectState> poll_updates();
		[[nodiscard]] bool apply(
		    const protocol::WorldObjectState &state,
		    std::string &error);
		void reset();
		[[nodiscard]] bool handle_entity_event(
		    Offsets::IEntity *entity,
		    void *raw_event);
		void entity_removed(Offsets::IEntity *entity);

	private:
		[[nodiscard]] std::optional<protocol::WorldObjectState> capture(
		    Offsets::IEntity *entity) const;
		[[nodiscard]] bool apply_inventory(
		    Offsets::IEntity *entity,
		    const protocol::WorldObjectState &state,
		    std::string &error) const;

		bool m_applying_world_state{};
		std::uint32_t m_poll_frame{};
		std::unordered_set<std::uint64_t> m_open_containers;
		std::unordered_map<std::uint64_t, protocol::WorldObjectState>
		    m_last_observations;
		std::unordered_map<std::uint64_t, protocol::WorldObjectState>
		    m_deferred_states;
		std::deque<protocol::WorldObjectState> m_updates;
	};
}
